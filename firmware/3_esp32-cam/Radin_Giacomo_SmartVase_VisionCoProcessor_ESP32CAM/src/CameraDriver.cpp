#include "CameraDriver.h"

bool cameraOk = false;

bool initCamera() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0  = Y2_GPIO_NUM;  c.pin_d1  = Y3_GPIO_NUM;
    c.pin_d2  = Y4_GPIO_NUM;  c.pin_d3  = Y5_GPIO_NUM;
    c.pin_d4  = Y6_GPIO_NUM;  c.pin_d5  = Y7_GPIO_NUM;
    c.pin_d6  = Y8_GPIO_NUM;  c.pin_d7  = Y9_GPIO_NUM;
    c.pin_xclk = XCLK_GPIO_NUM;
    c.pin_pclk = PCLK_GPIO_NUM;
    c.pin_vsync = VSYNC_GPIO_NUM;
    c.pin_href  = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM;
    c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn  = PWDN_GPIO_NUM;
    c.pin_reset = RESET_GPIO_NUM;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = PIXFORMAT_JPEG;
    if (psramFound()) {
        c.frame_size   = FRAMESIZE_SVGA;  // 800x600 resolution
        c.jpeg_quality = 12;
        c.fb_count     = 2;
    } else {
        c.frame_size   = FRAMESIZE_VGA;   // 640x480 resolution
        c.jpeg_quality = 14;
        c.fb_count     = 1;
    }
    esp_err_t err = esp_camera_init(&c);
    return err == ESP_OK;
}
