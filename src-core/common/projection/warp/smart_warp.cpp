#include "warp_bkd.h"

#include "logger.h"
#include "common/projection/projs/equirectangular.h"
#include "common/geodetic/vincentys_calculations.h"

namespace satdump
{
    namespace warp
    {
        void ensureMemoryLimit(satdump::warp::WarpCropSettings &crop_set, WarpOperation &operation_t, int nchannels, size_t mem_limit)
        {
        recheck_memory:
            size_t memory_usage = (size_t)abs(crop_set.x_min - crop_set.x_max) * (size_t)abs(crop_set.y_min - crop_set.y_max) * (size_t)nchannels * sizeof(uint16_t);

            if (memory_usage > mem_limit)
            {
                operation_t.output_height *= 0.9;
                operation_t.output_width *= 0.9;
                crop_set = choseCropArea(operation_t);
                // logger->critical("TOO MUCH MEMORY %llu", memory_usage);
                goto recheck_memory;
            }
        }

        int calculateSegmentNumberToSplitInto(WarpOperation &operation_t)
        {
            int nsegs = 1;
            std::vector<satdump::projection::GCP> gcps_curr = operation_t.ground_control_points;
            // Filter GCPs, only keep each first y in x
            std::sort(gcps_curr.begin(), gcps_curr.end(),
                      [&operation_t](auto &el1, auto &el2)
                      {
                          return el1.y * operation_t.input_image.width() + el1.x < el2.y * operation_t.input_image.width() + el2.x;
                      });
            {
                auto gcps_curr_bkp = gcps_curr;
                gcps_curr.clear();
                for (int y = 0; y < gcps_curr_bkp.size() - 1; y++)
                {
                    auto gcp1 = gcps_curr[y];
                    auto gcp2 = gcps_curr[y + 1];
                    if (gcp1.y != gcp2.y)
                        gcps_curr.push_back(gcp2);
                }
            }

            std::vector<double> distances;
            for (int y = 0; y < gcps_curr.size() - 1; y++)
            {
                auto gcp1 = gcps_curr[y];
                auto gcp2 = gcps_curr[y + 1];

                auto gcp_dist = geodetic::vincentys_inverse(geodetic::geodetic_coords_t(gcp1.lat, gcp1.lon, 0), geodetic::geodetic_coords_t(gcp2.lat, gcp2.lon, 0));
                distances.push_back(gcp_dist.distance);
            }

            std::sort(distances.begin(), distances.end());

            double media_dist = distances[distances.size() / 2];

            nsegs = int((media_dist * gcps_curr.size()) / 3000);
            if (nsegs == 0)
                nsegs = 1;

            logger->trace("We will split into %d segments. Median distance is %f km and total (avg) distance is %f km", nsegs, media_dist, media_dist * gcps_curr.size());

            return nsegs;
        }

        struct SegmentConfig
        {
            int y_start;
            int y_end;
            int shift_lon;
            int shift_lat;
            std::vector<satdump::projection::GCP> gcps;

            std::shared_ptr<projection::VizGeorefSpline2D> tps = nullptr;
        };

        void computeGCPCenter(std::vector<satdump::projection::GCP> &gcps, double &lon, double &lat)
        {
            double x_total = 0;
            double y_total = 0;
            double z_total = 0;

            for (auto &pt : gcps)
            {
                x_total += cos(pt.lat * DEG_TO_RAD) * cos(pt.lon * DEG_TO_RAD);
                y_total += cos(pt.lat * DEG_TO_RAD) * sin(pt.lon * DEG_TO_RAD);
                z_total += sin(pt.lat * DEG_TO_RAD);
            }

            x_total /= gcps.size();
            y_total /= gcps.size();
            z_total /= gcps.size();

            lon = atan2(y_total, x_total) * RAD_TO_DEG;
            double hyp = sqrt(x_total * x_total + y_total * y_total);
            lat = atan2(z_total, hyp) * RAD_TO_DEG;
        }

        void updateGCPOverlap(WarpOperation &operation_t, SegmentConfig &scfg, bool start_overlap, bool end_overlap)
        {
            int overlap = 0;
        find_second_gcp:
            int min_gcp_diff_start = std::numeric_limits<int>::max(); // Find last GCP before start
            int min_gcp_diff_end = std::numeric_limits<int>::max();   // Find first GCP after end
            for (auto gcp : operation_t.ground_control_points)
            {
                int diffs = scfg.y_start - gcp.y;
                if (diffs > 0 && diffs < min_gcp_diff_start)
                    min_gcp_diff_start = diffs;
                int diffe = gcp.y - scfg.y_end;
                if (diffe > 0 && diffe < min_gcp_diff_end)
                    min_gcp_diff_end = diffe;
            }

            if (min_gcp_diff_start != std::numeric_limits<int>::max() && start_overlap)
                scfg.y_start -= min_gcp_diff_start + 1;
            if (min_gcp_diff_end != std::numeric_limits<int>::max() && end_overlap)
                scfg.y_end += min_gcp_diff_end + 1;

            if (overlap++ < 1)
                goto find_second_gcp;

            if (scfg.y_start < 0)
                scfg.y_start = 0;
            if (scfg.y_end > operation_t.input_image.height())
                scfg.y_end = operation_t.input_image.height();
        }

        std::vector<SegmentConfig> prepareSegmentsAndSplitCuts(double nsegs, WarpOperation &operation_t)
        {
            std::vector<SegmentConfig> segmentConfigs;

            for (int segment = 0; segment < nsegs; segment++)
            {
                auto generateSeg = [&segmentConfigs, &operation_t](int start, int end, bool start_overlap, bool end_overlap)
                {
                    SegmentConfig scfg;

                    scfg.y_start = start;
                    scfg.y_end = end;

                    // Compute overlap if necessary
                    updateGCPOverlap(operation_t, scfg, start_overlap, end_overlap);

                    // Keep only GCPs for this segment
                    auto gcps_old = operation_t.ground_control_points;
                    for (auto gcp : gcps_old)
                    {
                        if (gcp.y >= scfg.y_start && gcp.y < scfg.y_end)
                        {
                            gcp.y -= scfg.y_start;
                            scfg.gcps.push_back(gcp);
                        }
                    }

                    // Calculate center, and handle longitude shifting
                    {
                        double center_lat = 0, center_lon = 0;
                        computeGCPCenter(scfg.gcps, center_lon, center_lat);
                        scfg.shift_lon = -center_lon;
                        scfg.shift_lat = 0;
                    }

                    // Check for GCPs near the poles. If any is close, it means this segment needs to be handled as a pole!
                    for (auto gcp : scfg.gcps)
                    {
                        auto south_dis = geodetic::vincentys_inverse(geodetic::geodetic_coords_t(gcp.lat, gcp.lon, 0), geodetic::geodetic_coords_t(-90, 0, 0));
                        auto north_dis = geodetic::vincentys_inverse(geodetic::geodetic_coords_t(gcp.lat, gcp.lon, 0), geodetic::geodetic_coords_t(90, 0, 0));

                        if (south_dis.distance < 1000)
                        {
                            scfg.shift_lon = 0;
                            scfg.shift_lat = -90;
                        }
                        if (north_dis.distance < 1000)
                        {
                            scfg.shift_lon = 0;
                            scfg.shift_lat = 90;
                        }
                    }

                    segmentConfigs.push_back(scfg);
                };

                // Calculate start / end
                int y_start = (segment / nsegs) * operation_t.input_image.height();
                int y_end = ((segment + 1) / nsegs) * operation_t.input_image.height();

                // Isolate GCPs for this segment
                std::vector<satdump::projection::GCP> gcps_curr;
                for (auto gcp : operation_t.ground_control_points)
                {
                    if (gcp.y >= y_start && gcp.y < y_end)
                    {
                        // gcp.y -= y_start;
                        gcps_curr.push_back(gcp);
                    }
                }

                // Filter GCPs, only keep each first y in x
                std::sort(gcps_curr.begin(), gcps_curr.end(),
                          [&operation_t](auto &el1, auto &el2)
                          {
                              return el1.y * operation_t.input_image.width() + el1.x < el2.y * operation_t.input_image.width() + el2.x;
                          });
                {
                    auto gcps_curr_bkp = gcps_curr;
                    gcps_curr.clear();
                    for (int y = 0; y < gcps_curr_bkp.size() - 1; y++)
                    {
                        auto gcp1 = gcps_curr[y];
                        auto gcp2 = gcps_curr[y + 1];
                        if (gcp1.y != gcp2.y)
                            gcps_curr.push_back(gcp2);
                    }
                }

                // Check if this segment is cut (eg, los of signal? Different recorded dump?)
                int cutPosition = -1;
                for (int y = 0; y < gcps_curr.size() - 1; y++)
                    if (geodetic::vincentys_inverse(geodetic::geodetic_coords_t(gcps_curr[y].lat, gcps_curr[y].lon, 0),
                                                    geodetic::geodetic_coords_t(gcps_curr[y + 1].lat, gcps_curr[y + 1].lon, 0))
                            .distance > 2000)
                        cutPosition = gcps_curr[y + 1].y;

                // Generate, handling cuts
                if (cutPosition != -1)
                {
                    generateSeg(y_start, cutPosition, true, false);
                    generateSeg(cutPosition, y_end, false, true);
                }
                else
                {
                    generateSeg(y_start, y_end, true, true);
                }
            }

            return segmentConfigs;
        }

        WarpResult performSmartWarp(WarpOperation operation_t, float *progress)
        {
            WarpResult result; // Final output

            // Prepare crop area, and check it can fit in RAM
            satdump::warp::WarpCropSettings crop_set = choseCropArea(operation_t);
            int nchannels = operation_t.output_rgba ? 4 : operation_t.input_image.channels();

            ensureMemoryLimit(crop_set, operation_t, nchannels, 4e9);

            // Prepare the output
            result.output_image = image::Image<uint16_t>(crop_set.x_max - crop_set.x_min, crop_set.y_max - crop_set.y_min,
                                                         nchannels);
            result.top_left = {0, 0, (double)crop_set.lon_min, (double)crop_set.lat_max};                                                                                  // 0,0
            result.top_right = {(double)result.output_image.width() - 1, 0, (double)crop_set.lon_max, (double)crop_set.lat_max};                                           // 1,0
            result.bottom_left = {0, (double)result.output_image.height() - 1, (double)crop_set.lon_min, (double)crop_set.lat_min};                                        // 0,1
            result.bottom_right = {(double)result.output_image.width() - 1, (double)result.output_image.height() - 1, (double)crop_set.lon_max, (double)crop_set.lat_min}; // 1,1

            // Prepare projection to draw segments
            geodetic::projection::EquirectangularProjection projector_final;
            projector_final.init(result.output_image.width(), result.output_image.height(), result.top_left.lon, result.top_left.lat, result.bottom_right.lon, result.bottom_right.lat);

            /// Try to calculate the number of segments to split the data into. All an approximation, but it's good enough!
            int nsegs = calculateSegmentNumberToSplitInto(operation_t);

            // Generate all segments
            std::vector<SegmentConfig> segmentConfigs = prepareSegmentsAndSplitCuts(nsegs, operation_t);

#pragma omp parallel for
            //  Solve all TPS transforms, multithreaded
            for (int ns = 0; ns < segmentConfigs.size(); ns++)
                segmentConfigs[ns].tps = initTPSTransform(segmentConfigs[ns].gcps, segmentConfigs[ns].shift_lon, segmentConfigs[ns].shift_lat);

            int scnt = 0;
            // Process all the segments
            for (auto &segmentCfg : segmentConfigs)
            {
                // Copy operation for the segment Warp
                auto operation = operation_t;
                operation.input_image.crop(0, segmentCfg.y_start, operation.input_image.width(), segmentCfg.y_end);
                operation.shift_lon = segmentCfg.shift_lon;
                operation.shift_lat = segmentCfg.shift_lat;
                operation.ground_control_points = segmentCfg.gcps;

                // Perform the actual warp
                satdump::warp::ImageWarper warper;
                warper.op = operation;
                warper.set_tps(segmentCfg.tps);
                warper.update(true);

                satdump::warp::WarpResult result2 = warper.warp();

                // Setup projector....
                geodetic::projection::EquirectangularProjection projector;
                projector.init(result2.output_image.width(), result2.output_image.height(),
                               result2.top_left.lon, result2.top_left.lat,
                               result2.bottom_right.lon, result2.bottom_right.lat);

#if 0 // Draw GCPs, useful for debug
                {
                    unsigned short color[4] = {0, 65535, 0, 65535};
                    for (auto gcp : operation.ground_control_points)
                    {
                        auto projfunc = [&projector](float lat, float lon, int, int) -> std::pair<int, int>
                        {
                            int x, y;
                            projector.forward(lon, lat, x, y);
                            return {x, y};
                        };

                        std::pair<int, int> pos = projfunc(gcp.lat, gcp.lon, 0, 0);

                        if (pos.first == -1 || pos.second == -1)
                            continue;

                        result2.output_image.draw_circle(pos.first, pos.second, 5, color, true);
                    }
                }
#endif

                // .....and re-project! (just a basic affine transform)
                float lon = result2.top_left.lon, lat = result2.top_left.lat;
                int x2 = 0, y2 = 0;
                projector_final.forward(lon, lat, x2, y2);
                if (!(x2 == -1 || y2 == -1))
                {
                    // Slightly modified draw_image()
                    int width = std::min<int>(result.output_image.width(), x2 + result2.output_image.width()) - x2;
                    int height = std::min<int>(result.output_image.height(), y2 + result2.output_image.height()) - y2;

                    if (result2.output_image.channels() == result.output_image.channels())
                        for (int x = 0; x < width; x++)
                            for (int y = 0; y < height; y++)
                                if (y + y2 >= 0 && x + x2 >= 0)
                                    if (result2.output_image.channel(3)[y * result2.output_image.width() + x] > 0)
                                    {
                                        for (int ch = 0; ch < 3; ch++)
                                            result.output_image.channel(ch)[(y + y2) * result.output_image.width() + x + x2] = result2.output_image.channel(ch)[y * result2.output_image.width() + x];
                                        result.output_image.channel(3)[(y + y2) * result.output_image.width() + x + x2] = 65535;
                                    }
                }

                scnt++;
                if (progress != nullptr)
                    *progress = (float)scnt / (float)segmentConfigs.size();

                /////////// DEBUG
                // result.output_image.save_img("projtest/test" + std::to_string((scnt) + 1));
            }

            return result;
        }
    }
}