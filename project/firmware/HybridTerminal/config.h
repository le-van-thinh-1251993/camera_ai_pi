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
static constexpr uint16_t LAN_DISCOVERY_PORT = 8002;  // UDP, xem app/lan_discovery.py
// Hardware flags for ESP32_BASE_BOARD_V1.2 — matched to schematic pinout.
// Compile override -D flags are useful for checking disabled driver branches.
#ifndef ENABLE_CAMERA
#define ENABLE_CAMERA 1  // OV3660 live; chay luan phien voi TFT (xem live_camera.h)
#endif
// 0 = bo DA SUA: TFT tach khoi camera (SCK=1, MOSI=2, DC=46, CS=GND, RST=EN).
// 1 = PCB goc chua sua: TFT chung GPIO 8-12 voi camera, chay luan phien (cham).
#ifndef SHARED_CAM_TFT_BUS
#define SHARED_CAM_TFT_BUS 0
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
// ===== CAMERA OV3660 - ESP32-S3 WROOM (FPC on board) =====
// Theo Bang_Ket_Noi_Module_Schematic.xlsx, dong 31-46.
// Y2..Y9 = CAM_D0..CAM_D7 (Y2 la bit thap nhat).
//
// Tren PCB goc Y2..Y6 (GPIO 11, 9, 8, 10, 12) noi chung voi TFT. Sau khi sua bo
// (SHARED_CAM_TFT_BUS 0) cac chan nay chi con noi camera.
#define PWDN_GPIO_NUM     -1   // FPC pin 8  - noi GND tren bo, khong dieu khien
#define RESET_GPIO_NUM    -1   // FPC pin 6  - keo len 3.3V tren bo
#define XCLK_GPIO_NUM     15   // FPC pin 13
#define SIOD_GPIO_NUM      4   // FPC pin 3  - SCCB SDA
#define SIOC_GPIO_NUM      5   // FPC pin 5  - SCCB SCL

#define Y9_GPIO_NUM       16   // D7 - FPC pin 12
#define Y8_GPIO_NUM       17   // D6 - FPC pin 14
#define Y7_GPIO_NUM       18   // D5 - FPC pin 16
#define Y6_GPIO_NUM       12   // D4 - FPC pin 18 - TRUNG TFT_SCK
#define Y5_GPIO_NUM       10   // D3 - FPC pin 20 - TRUNG TFT_DC
#define Y4_GPIO_NUM        8   // D2 - FPC pin 22 - TRUNG TFT_RST
#define Y3_GPIO_NUM        9   // D1 - FPC pin 21 - TRUNG TFT_CS
#define Y2_GPIO_NUM       11   // D0 - FPC pin 19 - TRUNG TFT_MOSI
#define VSYNC_GPIO_NUM     6   // FPC pin 7
#define HREF_GPIO_NUM      7   // FPC pin 9
#define PCLK_GPIO_NUM     13   // FPC pin 17

static constexpr int CAM_XCLK_HZ = 20000000;

// Live preview: QVGA 320x240 ve 1:1 len TFT 480x320, cung khung do phat MJPEG.
#define LIVE_FRAME_SIZE FRAMESIZE_QVGA  // QVGA 320x240 ~5 fps | HQVGA 240x176 ~9 fps (SPI 10 MHz)
#define CAPTURE_FRAME_SIZE FRAMESIZE_VGA  // anh gui Pi nhan dien (640x480)
static constexpr uint32_t AUTO_HOLD_MS = 2000;        // dem nguoc truoc khi tu chup
static constexpr uint32_t AUTO_NEXT_ROUND_MS = 8000;  // giu ket qua roi quay lai live; 0 = cho SELECT
static constexpr int LIVE_JPEG_QUALITY = 8;     // so nho = net hon (it vet khoi), QVGA ~10-15 KB
static constexpr int CAM_SHARPNESS = 2;         // -2..2 (OV3660): tang do sac net
static constexpr int CAM_DENOISE = 3;           // 0..8: giam hat nhieu (cao qua -> mo chi tiet)
#define CAM_GAINCEILING GAINCEILING_16X         // 4X: it hat nhung toi; 16X: sang hon; 32X+: rat hat
static constexpr int CAM_AE_LEVEL = 1;          // -2..2: bu phoi sang (+ = sang hon)
static constexpr int CAM_BRIGHTNESS = 1;        // -2..2
static constexpr uint16_t LIVE_HTTP_PORT = 80;  // /status, /capture; stream o cong 81

// Xoay 180 do = lat doc + lat ngang (ap dung ca TFT, stream va anh gui Pi)
static constexpr bool CAM_VFLIP   = true;
static constexpr bool CAM_HMIRROR = true;

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

// ===== LOA I2S MAX98357A - xlsx dong 10-17 =====
// SD noi 3.3V (luon bat), GAIN de trong (9 dB mac dinh).
static constexpr int AUDIO_BCLK  = 41;  // BCLK
static constexpr int AUDIO_LRCLK = 42;  // LRC (WS)
static constexpr int AUDIO_DOUT  = 45;  // DIN  - GPIO45 la chan strapping (VDD_SPI)
static constexpr int AUDIO_MCLK  = -1;  // MAX98357A khong dung MCLK
static constexpr int AUDIO_GAIN_PERCENT=60; // Digital gain, 0..100
static constexpr bool BUTTON_ACTIVE_LOW=true; // Input pull-up; button to GND after pins verified

// ===== MICRO I2S INMP441 - xlsx dong 18-23 =====
// L/R noi GND -> kenh trai. Hien chua dung (khong ghi am).
static constexpr int MIC_BCLK  = 39;  // SCK
static constexpr int MIC_LRCLK = 40;  // WS
static constexpr int MIC_DIN   = 38;  // SD

// ===== NUT BO & LED TRANG THAI - xlsx dong 28-30 =====
static constexpr int BOARD_BOOT_PIN  = 0;   // BOOT (strapping)
static constexpr int BOARD_LED_PIN   = 2;   // LED don (do) - bo da sua: dung lam TFT MOSI
static constexpr int BOARD_RGB_PIN   = -1;  // WS2812 tren GPIO48 - bo da sua dung GPIO48 lam TFT DC nen khong dieu khien LED

// ===== USB / UART - xlsx dong 52-55 (khong dung lam GPIO) =====
// UART0 TX=43, RX=44; USB OTG D-=19, D+=20.
static constexpr unsigned DEBOUNCE_MS=35;
static constexpr unsigned PREVIEW_INTERVAL_MS=100;
#define CLOUD_HTTP_TIMEOUT_MS 8000
static constexpr unsigned RESULT_TIMEOUT_MS=25000;
static constexpr size_t MAX_JPEG_BYTES=180000, MAX_SCREEN_BYTES=180000, MAX_WAV_BYTES=512000;
