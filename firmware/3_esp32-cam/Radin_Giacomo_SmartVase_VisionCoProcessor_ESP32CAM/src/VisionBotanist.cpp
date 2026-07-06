#include "VisionBotanist.h"
#include "ConfigManager.h"
#include <math.h>

//analyze camera framebuffer colors to evaluate plant health and coverage
AnalysisResult doAnalysis(camera_fb_t* fb) {
    AnalysisResult res = {false, 0, 0, 0, 0.0f, 0.0f, 0.0f, true, "Cannot analyze frame (invalid buffer)"};
    //return immediately if camera buffer is empty or invalid
    if (!fb || fb->len == 0) return res;

    //allocate external psram memory buffer to decode jpeg into rgb
    uint8_t* rgb_buf = (uint8_t*)heap_caps_malloc(fb->width * fb->height * 3, MALLOC_CAP_SPIRAM);
    if (!rgb_buf) {
        res.status_message = "Insufficient PSRAM memory for RGB decoding";
        return res;
    }

    //decode compressed jpeg frame into raw rgb888 pixel format
    bool decoded = fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf);
    if (decoded) {
        uint32_t cx = cfg.roi_center_x;
        uint32_t cy = cfg.roi_center_y;
        uint32_t r_sq = (uint32_t)cfg.roi_radius * (uint32_t)cfg.roi_radius;

        //iterate over all image pixels to evaluate circular region of interest
        for (uint32_t y = 0; y < fb->height; y++) {
            uint32_t dy = (y > cy) ? (y - cy) : (cy - y);
            for (uint32_t x = 0; x < fb->width; x++) {
                uint32_t dx = (x > cx) ? (x - cx) : (cx - x);
                //skip pixels outside the configured circular region of interest
                if (dx * dx + dy * dy > r_sq) {
                    continue;
                }

                res.total_roi_pixels++;

                uint32_t idx = (y * fb->width + x) * 3;
                float rf = rgb_buf[idx] / 255.0f;
                float gf = rgb_buf[idx+1] / 255.0f;
                float bf = rgb_buf[idx+2] / 255.0f;

                float cmax = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
                float cmin = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
                float delta = cmax - cmin;

                float h = 0.0f;
                if (delta > 0.0001f) {
                    if (cmax == rf)      h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
                    else if (cmax == gf) h = 60.0f * (((bf - rf) / delta) + 2.0f);
                    else                 h = 60.0f * (((rf - gf) / delta) + 4.0f);
                    if (h < 0.0f) h += 360.0f;
                }
                float s = (cmax > 0.0001f) ? (delta / cmax) : 0.0f;
                float v = cmax;

                //count healthy green basil foliage pixels based on hue and saturation
                if (h >= 35.0f && h <= 95.0f && s >= 0.20f && v >= 0.15f) {
                    res.green_pixels++;
                }
                //count dry or yellowing brown foliage pixels
                else if (h >= 10.0f && h < 35.0f && s >= 0.25f && v >= 0.15f) {
                    res.brown_pixels++;
                }
            }
        }

        //calculate foliage ratios and determine overall plant health status
        if (res.total_roi_pixels > 0) {
            res.green_ratio = (float)res.green_pixels / res.total_roi_pixels;
            res.brown_ratio = (float)res.brown_pixels / res.total_roi_pixels;
            res.foliage_coverage = (float)(res.green_pixels + res.brown_pixels) / res.total_roi_pixels;
            res.valid = true;

            //set warning status if plant is missing or showing signs of sickness
            if (res.foliage_coverage < 0.05f) {
                res.plant_healthy = false;
                res.status_message = "Warning: Very low foliage coverage (possible wilting or missing plant)!";
            } else if (res.brown_pixels > res.green_pixels * 0.25f) {
                res.plant_healthy = false;
                res.status_message = "Warning: Detected dry, yellowing, or sick foliage!";
            } else {
                res.plant_healthy = true;
                res.status_message = "Healthy: Basil plant has good turgor and green foliage!";
            }
        } else {
            res.status_message = "Invalid circular ROI (0 pixels evaluated)";
        }
    } else {
        res.status_message = "JPEG to RGB888 frame decoding failed";
    }

    //release allocated psram rgb memory buffer
    heap_caps_free(rgb_buf);
    return res;
}
