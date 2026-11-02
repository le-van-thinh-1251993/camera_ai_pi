#pragma once
// Copy network values here. Never publish this file after adding credentials.
static constexpr char WIFI_SSID[] = "";
static constexpr char WIFI_PASSWORD[] = "";
static constexpr char DEVICE_ID[] = "esp32-01";
static constexpr char LAN_HOSTNAME[] = "ai-camera.local";
static constexpr uint16_t LAN_PORT = 8000;
#ifndef LAN_API_KEY_VALUE
#define LAN_API_KEY_VALUE ""
#endif

// All hardware stays OFF until the actual board, driver and pin map are known.
// Compile override -D flags are useful for checking disabled driver branches.
#ifndef ENABLE_CAMERA
#define ENABLE_CAMERA 0
#endif
#ifndef TFT_KIND
#define TFT_KIND 0 // 0=Serial only, 1=ILI9488 SPI, 2=ST7796 SPI, 3=ILI9341 SPI
#endif
#ifndef ENABLE_I2S_AUDIO
#define ENABLE_I2S_AUDIO 0 // Adapter for a standard I2S amp; not an analog/PWM amp
#endif
static constexpr int CAM_D0=-1,CAM_D1=-1,CAM_D2=-1,CAM_D3=-1;
static constexpr int CAM_D4=-1,CAM_D5=-1,CAM_D6=-1,CAM_D7=-1;
static constexpr int CAM_XCLK=-1,CAM_PCLK=-1,CAM_VSYNC=-1,CAM_HREF=-1;
static constexpr int CAM_SDA=-1,CAM_SCL=-1,CAM_PWDN=-1,CAM_RESET=-1;
static constexpr int CAM_XCLK_HZ=20000000; // Confirm against camera module datasheet
static constexpr bool CAM_VFLIP=false,CAM_HMIRROR=false;

static constexpr int TFT_SCLK=-1,TFT_MOSI=-1,TFT_MISO=-1,TFT_CS=-1,TFT_DC=-1,TFT_RST=-1;
static constexpr int TFT_BL=-1; // Optional external backlight enable, not a power supply
static constexpr bool TFT_BL_ACTIVE_HIGH=true;
static constexpr int TFT_NATIVE_WIDTH=320,TFT_NATIVE_HEIGHT=480; // ILI9488, rotation 1 -> 480x320
static constexpr int TFT_ROTATION=1;
static constexpr bool TFT_INVERT=false,TFT_RGB_ORDER=false;
static constexpr int TFT_SPI_HZ=20000000;

static constexpr int AUDIO_BCLK=-1,AUDIO_LRCLK=-1,AUDIO_DOUT=-1;
static constexpr int AUDIO_MCLK=-1; // Optional; check amplifier
static constexpr int AUDIO_GAIN_PERCENT=60; // Digital gain, 0..100
static constexpr int BTN_HOME=-1,BTN_UP=-1,BTN_DOWN=-1,BTN_SELECT=-1;
static constexpr bool BUTTON_ACTIVE_LOW=true; // Input pull-up; button to GND after pins verified
// INMP441 is reserved. Gesture workflow does not record audio or run speech recognition.
static constexpr int MIC_BCLK=-1,MIC_LRCLK=-1,MIC_DIN=-1;
static constexpr unsigned DEBOUNCE_MS=35;
static constexpr unsigned PREVIEW_INTERVAL_MS=100;
#define CLOUD_HTTP_TIMEOUT_MS 8000
static constexpr size_t MAX_JPEG_BYTES=180000, MAX_SCREEN_BYTES=180000, MAX_WAV_BYTES=512000;
