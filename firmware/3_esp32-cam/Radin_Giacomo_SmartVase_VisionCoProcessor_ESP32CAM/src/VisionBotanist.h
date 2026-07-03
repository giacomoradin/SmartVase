#ifndef VISION_BOTANIST_H
#define VISION_BOTANIST_H

#include <Arduino.h>
#include "esp_camera.h"

struct AnalysisResult {
    bool valid;
    uint32_t green_pixels;
    uint32_t brown_pixels;
    uint32_t total_roi_pixels;
    float green_ratio;
    float brown_ratio;
    float foliage_coverage;
    bool plant_healthy;
    String status_message;
};

AnalysisResult doAnalysis(camera_fb_t* fb);

#endif // VISION_BOTANIST_H
