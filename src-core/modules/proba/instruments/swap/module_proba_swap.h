#pragma once

#include "module.h"

namespace proba
{
    namespace swap
    {
        class ProbaSWAPDecoderModule : public ProcessingModule
        {
        protected:
            std::atomic<size_t> filesize;
            std::atomic<size_t> progress;

        public:
            ProbaSWAPDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            void process();
            void drawUI(bool window);

        public:
            static std::string getID();
            virtual std::string getIDM() { return getID(); };
            static std::vector<std::string> getParameters();
            static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        };
    } // namespace avhrr
} // namespace noaa