#include "radiation_handler.h"
#include "core/config.h"
#include "resources.h"
#include "core/module.h"

namespace satdump
{
    void RadiationViewerHandler::init()
    {
        products = (RadiationProducts *)ViewerHandler::products;

        for (int c = 0; c < (int)products->channel_counts.size(); c++)
            select_channel_image_str += products->get_channel_name(c) + '\0';

        update();
    }

    void RadiationViewerHandler::update()
    {
        if (selected_visualization_id == 0)
        {

            RadiationMapCfg cfg;
            cfg.channel = select_channel_image_id + 1;
            cfg.radius = 5;
            cfg.min = map_min;
            cfg.max = map_max;
            image::Image<uint16_t> map = make_radiation_map(*products, cfg);
            image_view.update(map);
        }
        else if (selected_visualization_id == 1)
        {
            graph_values.clear();
            graph_values.resize(products->channel_counts.size());
            for (int c = 0; c < (int)products->channel_counts.size(); c++)
            {
                for (int i = 0; i < (int)products->channel_counts[c].size(); i++)
                    graph_values[c].push_back(products->channel_counts[c][i]);
            }
        }
    }

    void RadiationViewerHandler::drawMenu()
    {
        if (ImGui::CollapsingHeader("Vizualisation"))
        {
            if (ImGui::RadioButton(u8"\uf84c   Map", &selected_visualization_id, 0))
                update();
            if (ImGui::RadioButton(u8"\uf437   Graph", &selected_visualization_id, 1))
                update();

            if (selected_visualization_id == 0)
            {
                if (ImGui::Combo("###mapchannelcomboid", &select_channel_image_id, select_channel_image_str.c_str()))
                    update();
                ImGui::SetNextItemWidth(ImGui::GetWindowWidth() / 2);
                if (ImGui::DragInt("##Min", &map_min, 1.0f, 0, 255, "Min: %d", ImGuiSliderFlags_AlwaysClamp))
                    update();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetWindowWidth() / 2);
                if (ImGui::DragInt("##Max", &map_max, 1.0f, 0, 255, "Max: %d", ImGuiSliderFlags_AlwaysClamp))
                    update();
            }
        }
    }

    void RadiationViewerHandler::drawContents(ImVec2 win_size)
    {
        if (selected_visualization_id == 0)
        {
            image_view.draw(win_size);
        }
        else if (selected_visualization_id == 1)
        {
            ImGui::BeginChild("RadiationPlot");
            for (int i = 0; i < (int)products->channel_counts.size(); i++)
            {
                ImGui::BeginChild(("RadiationPlotChild##" + std::to_string(i)).c_str(), ImVec2(ImGui::GetWindowWidth(), 50*ui_scale));
                ImGui::PlotLines(products->get_channel_name(i).c_str(), graph_values[i].data(), graph_values[i].size(), 0, NULL, 0, 255, ImVec2(ImGui::GetWindowWidth()-100*ui_scale, 30*ui_scale));
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::EndChild();
            }
            ImGui::EndChild();
            
        }
    }

    float RadiationViewerHandler::drawTreeMenu()
    {
        return 0;
    }
}