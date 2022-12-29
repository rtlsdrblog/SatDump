#pragma once

#include "products.h"
#include "common/image/image.h"

namespace satdump
{
    class ImageProducts : public Products
    {
    public:
        struct ImageHolder
        {
            std::string filename;
            std::string channel_name;
            image::Image<uint16_t> image;
            std::vector<double> timestamps = std::vector<double>();
            int ifov_y = -1;
            int ifov_x = -1;
            int offset_x = 0;
        };

        std::vector<ImageHolder> images;

        bool has_timestamps = true;
        bool needs_correlation = false;
        int ifov_y = -1;
        int ifov_x = -1;

        int bit_depth = 16;

        bool save_as_matrix = false;

        ///////////////////////// Timestamps

        enum Timestamp_Type
        {
            TIMESTAMP_LINE,
            TIMESTAMP_MULTIPLE_LINES,
            TIMESTAMP_IFOV,
        };

        Timestamp_Type timestamp_type;

        void set_timestamps(std::vector<double> timestamps)
        {
            contents["timestamps"] = timestamps;
        }

        std::vector<double> get_timestamps(int image_index = -1)
        {
            try
            {
                if (image_index == -1)
                    return contents["timestamps"].get<std::vector<double>>();

                if ((int)images.size() > image_index)
                {
                    if (images[image_index].timestamps.size() > 0)
                        return images[image_index].timestamps;
                    else
                        return contents["timestamps"].get<std::vector<double>>();
                }
                else
                    return contents["timestamps"].get<std::vector<double>>();
            }
            catch (std::exception &e)
            {
                return {};
            }
        }

        int get_ifov_y_size(int image_index = -1)
        {
            if (images[image_index].ifov_y != -1)
                return images[image_index].ifov_y;
            else
                return ifov_y;
        }

        int get_ifov_x_size(int image_index = -1)
        {
            if (images[image_index].ifov_x != -1)
                return images[image_index].ifov_x;
            else
                return ifov_x;
        }

        ///////////////////////// Projection

        void set_proj_cfg(nlohmann::ordered_json cfg)
        {
            contents["projection_cfg"] = cfg;
        }

        nlohmann::ordered_json get_proj_cfg()
        {
            return contents["projection_cfg"];
        }

        nlohmann::ordered_json get_channel_proj_metdata(int ch)
        {
            nlohmann::ordered_json mtd;
            if (images[ch].offset_x != 0)
                mtd["img_offset_x"] = images[ch].offset_x;
            return mtd;
        }

        bool has_proj_cfg()
        {
            return contents.contains("projection_cfg");
        }

        bool can_geometrically_correct()
        {
            if (!has_proj_cfg())
                return false;
            if (!contents.contains("projection_cfg"))
                return false;
            if (!get_proj_cfg().contains("corr_swath"))
                return false;
            if (!get_proj_cfg().contains("corr_resol"))
                return false;
            if (!get_proj_cfg().contains("corr_altit"))
                return false;
            return true;
        }

        /// CALIBRATION

        bool has_calibation()
        {
            return contents.contains("calibration");
        }

        void set_calibration(nlohmann::json calib)
        {
            bool d = false;
            nlohmann::json buff;
            if (has_calibation() && contents["calibration"].contains("wavenumbers"))
            {
                buff = contents["calibration"]["wavenumbers"];
                d = true;
            }
            contents["calibration"] = calib;
            if (d)
                contents["calibration"]["wavenumbers"] = buff;
        }

        double get_wavenumber(int image_index)
        {
            if (!has_calibation())
                return 0;

            if (contents["calibration"].contains("wavenumbers"))
                return contents["calibration"]["wavenumbers"][image_index].get<double>();
            else
                return 0;
        }

        void set_wavenumber(int image_index, double wavnb)
        {
            contents["calibration"]["wavenumbers"][image_index] = wavnb;
        }

        double get_radiance_value(int image_index, int x, int y);

    public:
        virtual void save(std::string directory);
        virtual void load(std::string file);

        ~ImageProducts()
        {
            if (lua_state_ptr != nullptr)
                delete lua_state_ptr;
        }

    private:
        void *lua_state_ptr = nullptr; // Opaque pointer to not include sol2 here... As it's big!
    };

    // Composite handling
    struct ImageCompositeCfg
    {
        std::string equation;
        bool equalize = false;
        bool invert = false;
        bool normalize = false;
        bool white_balance = false;

        std::string lut = "";
        std::string lut_channels = "";
    };

    inline void to_json(nlohmann::json &j, const ImageCompositeCfg &v)
    {
        j["equation"] = v.equation;
        j["equalize"] = v.equalize;
        j["invert"] = v.invert;
        j["normalize"] = v.normalize;
        j["white_balance"] = v.white_balance;

        j["lut"] = v.lut;
        j["lut_channels"] = v.lut_channels;
    }

    inline void from_json(const nlohmann::json &j, ImageCompositeCfg &v)
    {
        if (j.contains("equation"))
        {
            v.equation = j["equation"].get<std::string>();
        }
        else if (j.contains("lut"))
        {
            v.lut = j["lut"].get<std::string>();
            v.lut_channels = j["lut_channels"].get<std::string>();
        }

        if (j.contains("equalize"))
            v.equalize = j["equalize"].get<bool>();
        if (j.contains("invert"))
            v.invert = j["invert"].get<bool>();
        if (j.contains("normalize"))
            v.normalize = j["normalize"].get<bool>();
        if (j.contains("white_balance"))
            v.white_balance = j["white_balance"].get<bool>();
    }

    image::Image<uint16_t> make_composite_from_product(ImageProducts &product, ImageCompositeCfg cfg, float *progress = nullptr, std::vector<double> *final_timestamps = nullptr, nlohmann::json *final_metadata = nullptr);
    image::Image<uint16_t> perform_geometric_correction(ImageProducts &product, image::Image<uint16_t> img, bool &success, float *foward_table = nullptr);
}