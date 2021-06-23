#include "module_oqpsk_demod.h"
#include "common/dsp/lib/fir_gen.h"
#include "logger.h"
#include "imgui/imgui.h"

// Return filesize
size_t getFilesize(std::string filepath);

OQPSKDemodModule::OQPSKDemodModule(std::string input_file, std::string output_file_hint, std::map<std::string, std::string> parameters) : ProcessingModule(input_file, output_file_hint, parameters),
                                                                                                                                          d_agc_rate(std::stof(parameters["agc_rate"])),
                                                                                                                                          d_samplerate(std::stoi(parameters["samplerate"])),
                                                                                                                                          d_symbolrate(std::stoi(parameters["symbolrate"])),
                                                                                                                                          d_rrc_alpha(std::stof(parameters["rrc_alpha"])),
                                                                                                                                          d_rrc_taps(std::stoi(parameters["rrc_taps"])),
                                                                                                                                          d_loop_bw(std::stof(parameters["costas_bw"])),
                                                                                                                                          d_dc_block(std::stoi(parameters["dc_block"])),
                                                                                                                                          d_buffer_size(std::stoi(parameters["buffer_size"])),
                                                                                                                                          d_const_scale(std::stof(parameters["constellation_scale"])),
                                                                                                                                          d_iq_swap(parameters.count("iq_swap") > 0 ? std::stoi(parameters["iq_swap"]) : 0),
                                                                                                                                          d_clock_gain_omega(std::stof(parameters["clock_gain_omega"])),
                                                                                                                                          d_clock_mu(std::stof(parameters["clock_mu"])),
                                                                                                                                          d_clock_gain_mu(std::stof(parameters["clock_gain_mu"])),
                                                                                                                                          d_clock_omega_relative_limit(std::stof(parameters["clock_omega_relative_limit"])),
                                                                                                                                          constellation(100.0f / 127.0f, 100.0f / 127.0f, demod_constellation_size)

{
    // Buffers
    sym_buffer = new int8_t[d_buffer_size * 2];
    snr = 0;
}

void OQPSKDemodModule::init()
{
    float input_sps = (float)d_samplerate / (float)d_symbolrate;         // Compute input SPS
    resample = input_sps > MAX_SPS;                                      // If SPS is over MAX_SPS, we resample
    float samplerate = resample ? d_symbolrate * MAX_SPS : d_samplerate; // Get the final samplerate we'll be working with
    float decimation_factor = d_samplerate / samplerate;                 // Decimation factor to rescale our input buffer

    if (resample)
        d_buffer_size *= round(decimation_factor);

    float sps = samplerate / (float)d_symbolrate;

    logger->debug("Input SPS : " + std::to_string(input_sps));
    logger->debug("Resample : " + std::to_string(resample));
    logger->debug("Samplerate : " + std::to_string(samplerate));
    logger->debug("Dec factor : " + std::to_string(decimation_factor));
    logger->debug("Final SPS : " + std::to_string(sps));

    // Init DSP blocks
    if (input_data_type == DATA_FILE)
        file_source = std::make_shared<dsp::FileSourceBlock>(d_input_file, dsp::BasebandTypeFromString(d_parameters["baseband_format"]), d_buffer_size);
    if (d_dc_block)
        dcb = std::make_shared<dsp::DCBlockerBlock>(input_data_type == DATA_DSP_STREAM ? input_stream : file_source->output_stream, 1024, true);

    // Cleanup things a bit
    std::shared_ptr<dsp::stream<std::complex<float>>> input_data = d_dc_block ? dcb->output_stream : (input_data_type == DATA_DSP_STREAM ? input_stream : file_source->output_stream);

    // Init resampler if required
    if (resample)
        res = std::make_shared<dsp::CCRationalResamplerBlock>(input_data, samplerate, d_samplerate);

    // AGC
    agc = std::make_shared<dsp::AGCBlock>(resample ? res->output_stream : input_data, d_agc_rate, 1.0f, 1.0f, 65536);

    // RRC
    rrc = std::make_shared<dsp::CCFIRBlock>(agc->output_stream, 1, dsp::firgen::root_raised_cosine(1, samplerate, d_symbolrate, d_rrc_alpha, d_rrc_taps));

    // Costas
    pll = std::make_shared<dsp::CostasLoopBlock>(rrc->output_stream, d_loop_bw, 4);
    // Delay for OQPSK
    del = std::make_shared<dsp::DelayOneImagBlock>(pll->output_stream);

    // Clock recovery
    rec = std::make_shared<dsp::CCMMClockRecoveryBlock>(del->output_stream, sps, d_clock_gain_omega, d_clock_mu, d_clock_gain_mu, d_clock_omega_relative_limit);
}

std::vector<ModuleDataType> OQPSKDemodModule::getInputTypes()
{
    return {DATA_FILE, DATA_DSP_STREAM};
}

std::vector<ModuleDataType> OQPSKDemodModule::getOutputTypes()
{
    return {DATA_FILE, DATA_STREAM};
}

OQPSKDemodModule::~OQPSKDemodModule()
{
    delete[] sym_buffer;
}

void OQPSKDemodModule::process()
{
    if (input_data_type == DATA_FILE)
        filesize = file_source->getFilesize();
    else
        filesize = 0;

    if (output_data_type == DATA_FILE)
    {
        data_out = std::ofstream(d_output_file_hint + ".soft", std::ios::binary);
        d_output_files.push_back(d_output_file_hint + ".soft");
    }

    logger->info("Using input baseband " + d_input_file);
    logger->info("Demodulating to " + d_output_file_hint + ".soft");
    logger->info("Buffer size : " + std::to_string(d_buffer_size));

    time_t lastTime = 0;

    // Start
    if (input_data_type == DATA_FILE)
        file_source->start();
    if (d_dc_block)
        dcb->start();
    if (resample)
        res->start();
    agc->start();
    rrc->start();
    pll->start();
    del->start();
    rec->start();

    int dat_size = 0;
    while (input_data_type == DATA_FILE ? !file_source->eof() : input_active.load())
    {
        dat_size = rec->output_stream->read();

        if (dat_size <= 0)
            continue;

        // Estimate SNR, only on part of the samples to limit CPU usage
        snr_estimator.update(rec->output_stream->readBuf, dat_size / 100);
        snr = snr_estimator.snr();

        for (int i = 0; i < dat_size; i++)
        {
            sym_buffer[i * 2] = clamp(rec->output_stream->readBuf[i].real() * d_const_scale);
            sym_buffer[i * 2 + 1] = clamp(rec->output_stream->readBuf[i].imag() * d_const_scale);
        }

        rec->output_stream->flush();

        if (output_data_type == DATA_FILE)
            data_out.write((char *)sym_buffer, dat_size * 2);
        else
            output_fifo->write((uint8_t *)sym_buffer, dat_size * 2);

        if (input_data_type == DATA_FILE)
            progress = file_source->getPosition();
        if (time(NULL) % 10 == 0 && lastTime != time(NULL))
        {
            lastTime = time(NULL);
            logger->info("Progress " + std::to_string(round(((float)progress / (float)filesize) * 1000.0f) / 10.0f) + "%, SNR : " + std::to_string(snr) + "dB");
        }
    }

    logger->info("Demodulation finished");

    if (input_data_type == DATA_FILE)
        stop();
}

void OQPSKDemodModule::stop()
{
    // Stop
    if (input_data_type == DATA_FILE)
        file_source->stop();
    if (d_dc_block)
        dcb->stop();
    if (resample)
        res->stop();
    agc->stop();
    rrc->stop();
    pll->stop();
    del->stop();
    rec->stop();
    rec->output_stream->stopReader();

    if (output_data_type == DATA_FILE)
        data_out.close();
}

const ImColor colorNosync = ImColor::HSV(0 / 360.0, 1, 1, 1.0);
const ImColor colorSyncing = ImColor::HSV(39.0 / 360.0, 0.93, 1, 1.0);
const ImColor colorSynced = ImColor::HSV(113.0 / 360.0, 1, 1, 1.0);

void OQPSKDemodModule::drawUI(bool window)
{
    ImGui::Begin("OQPSK Demodulator", NULL, window ? NULL : NOWINDOW_FLAGS);

    ImGui::BeginGroup();
    constellation.pushComplex(rec->output_stream->readBuf, rec->output_stream->getDataSize());
    constellation.draw();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();
    {
        ImGui::Button("Signal", {200 * ui_scale, 20 * ui_scale});
        {
            ImGui::Text("SNR (dB) : ");
            ImGui::SameLine();
            ImGui::TextColored(snr > 2 ? snr > 10 ? colorSynced : colorSyncing : colorNosync, UITO_C_STR(snr));

            std::memmove(&snr_history[0], &snr_history[1], (200 - 1) * sizeof(float));
            snr_history[200 - 1] = snr;

            ImGui::PlotLines("", snr_history, IM_ARRAYSIZE(snr_history), 0, "", 0.0f, 25.0f, ImVec2(200 * ui_scale, 50 * ui_scale));
        }
    }
    ImGui::EndGroup();

    ImGui::ProgressBar((float)progress / (float)filesize, ImVec2(ImGui::GetWindowWidth() - 10, 20 * ui_scale));

    ImGui::End();
}

std::string OQPSKDemodModule::getID()
{
    return "oqpsk_demod";
}

std::vector<std::string> OQPSKDemodModule::getParameters()
{
    return {"samplerate", "symbolrate", "agc_rate", "rrc_alpha", "rrc_taps", "costas_bw", "iq_invert", "buffer_size", "clock_gain_omega", "clock_mu", "clock_gain_mu", "clock_omega_relative_limit", "constellation_scale", "baseband_format"};
}

std::shared_ptr<ProcessingModule> OQPSKDemodModule::getInstance(std::string input_file, std::string output_file_hint, std::map<std::string, std::string> parameters)
{
    return std::make_shared<OQPSKDemodModule>(input_file, output_file_hint, parameters);
}