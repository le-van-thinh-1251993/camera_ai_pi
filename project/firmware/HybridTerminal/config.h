#pragma once
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#ifndef WIFI_SSID_VALUE
#define WIFI_SSID_VALUE ""
#endif
#ifndef WIFI_PASSWORD_VALUE
#define WIFI_PASSWORD_VALUE ""
#endif
#ifndef LAN_API_KEY_VALUE
#define LAN_API_KEY_VALUE ""
#endif
static constexpr char WIFI_SSID[] = WIFI_SSID_VALUE;
static constexpr char WIFI_PASSWORD[] = WIFI_PASSWORD_VALUE;
static constexpr char DEVICE_ID[] = "esp32-01";
static constexpr char LAN_HOSTNAME[] = "ai-camera";
static constexpr uint16_t LAN_PORT = 8000;
// Hardware flags for ESP32_BASE_BOARD_V1.2 — matched to schematic pinout.
// Compile override -D flags are useful for checking disabled driver branches.
#ifndef ENABLE_CAMERA
#define ENABLE_CAMERA 0  // UI-only demo: camera disabled
#endif
#ifndef USE_35INCH_TFT
#define USE_35INCH_TFT 1  // ILI9488 3.5" SPI TFT confirmed on schematic
#endif
#if USE_35INCH_TFT
#include "config_35inch_tft.h"
#else
#ifndef TFT_KIND
#define TFT_KIND 0 // 0=Serial only, 1=ILI9488 SPI, 2=ST7796 SPI, 3=ILI9341 SPI
#endif
#endif
#ifndef ENABLE_I2S_AUDIO
#define ENABLE_I2S_AUDIO 1 // MAX98357A output enabled
#endif
// ===== CAMERA OV3660 - ESP32-S3 =====
static constexpr int CAM_D0 = 11;
static constexpr int CAM_D1 = 9;
static constexpr int CAM_D2 = 8;
static constexpr int CAM_D3 = 10;
static constexpr int CAM_D4 = 12;
static constexpr int CAM_D5 = 18;
static constexpr int CAM_D6 = 17;
static constexpr int CAM_D7 = 16;

static constexpr int CAM_XCLK  = 15;
static constexpr int CAM_PCLK  = 13;
static constexpr int CAM_VSYNC = 6;
static constexpr int CAM_HREF  = 7;

static constexpr int CAM_SDA   = 4;
static constexpr int CAM_SCL   = 5;
static constexpr int CAM_PWDN  = -1;
static constexpr int CAM_RESET = -1;

static constexpr int CAM_XCLK_HZ = 20000000;

static constexpr bool CAM_VFLIP   = false;
static constexpr bool CAM_HMIRROR = false;

#if !USE_35INCH_TFT
static constexpr int TFT_SCLK=-1,TFT_MOSI=-1,TFT_MISO=-1,TFT_CS=-1,TFT_DC=-1,TFT_RST=-1;
static constexpr int TFT_BL=-1; // Optional external backlight enable, not a power supply
static constexpr bool TFT_BL_ACTIVE_HIGH=true;
static constexpr int TFT_NATIVE_WIDTH=320,TFT_NATIVE_HEIGHT=480; // ILI9488, rotation 1 -> 480x320
static constexpr int TFT_ROTATION=1;
static constexpr bool TFT_INVERT=false,TFT_RGB_ORDER=false;
static constexpr int TFT_SPI_HZ=20000000;
#endif
#if !USE_35INCH_TFT
static constexpr int BTN_NEXT=-1,BTN_VOLUME_UP=-1,BTN_VOLUME_DOWN=-1,BTN_MANUAL=-1,BTN_POWER=-1;
#endif

static constexpr int AUDIO_BCLK=41,AUDIO_LRCLK=42,AUDIO_DOUT=45;
static constexpr int AUDIO_MCLK=-1; // MAX98357A does not use MCLK
static constexpr int AUDIO_GAIN_PERCENT=60; // Digital gain, 0..100
static constexpr bool BUTTON_ACTIVE_LOW=true; // Input pull-up; button to GND after pins verified
// INMP441 is reserved. Gesture workflow does not record audio or run speech recognition.
static constexpr int MIC_BCLK=39,MIC_LRCLK=40,MIC_DIN=38;  // Schematic: SCK=39, WS=40, SD=38
static constexpr unsigned DEBOUNCE_MS=35;
static constexpr unsigned PREVIEW_INTERVAL_MS=100;
#define CLOUD_HTTP_TIMEOUT_MS 8000
static constexpr unsigned RESULT_TIMEOUT_MS=25000;
static constexpr size_t MAX_JPEG_BYTES=180000, MAX_SCREEN_BYTES=180000, MAX_WAV_BYTES=512000;
