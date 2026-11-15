#pragma once

// 3.5-inch ILI9488 SPI TFT - theo Bang_Ket_Noi_Module_Schematic.xlsx dong 1-9.
// VCC = 5V sau cong tac, LED = 3.3V (AMS1117), SDO (MISO) de trong.
//
// SHARED_CAM_TFT_BUS (config.h) chon so do chan: PCB goc (chung bus camera)
// hoac bo da sua (TFT tach rieng, xem ben duoi).
#ifndef TFT_KIND
#define TFT_KIND 1  // ILI9488
#endif

static constexpr int TFT_NATIVE_WIDTH = 320;
static constexpr int TFT_NATIVE_HEIGHT = 480;
static constexpr int TFT_ROTATION = 3;

#if SHARED_CAM_TFT_BUS
// PCB GOC: TFT nam chung GPIO voi camera, live_camera.h chia thoi gian.
static constexpr int TFT_CS   = 9;    // CS          (trung camera Y3)
static constexpr int TFT_RST  = 8;    // RESET       (trung camera Y4)
static constexpr int TFT_DC   = 10;   // DC / RS     (trung camera Y5)
static constexpr int TFT_MOSI = 11;   // SDI (MOSI)  (trung camera Y2)
static constexpr int TFT_SCLK = 12;   // SCK         (trung camera Y6)
#else
// BO DA SUA: cat chan 3-7 cua socket TFT khoi GPIO 8-12, noi day lai:
//   SCK -> GPIO1, MOSI -> GPIO2, DC -> GPIO48, CS -> GND, RESET -> EN.
// Camera va TFT chay dong thoi, khong con chia thoi gian.
// CS noi GND + RESET noi EN => sau khi nap code (ESP32-S3 reset noi bo, EN khong doi) hoac
// co xung nhieu tren SCK luc khoi dong, man bi lech bit -> man TRANG cho toi khi rut dien.
// Sua dut diem: noi RESET (socket chan 4) sang GPIO40 va/hoac CS (chan 3) sang GPIO39
// (2 chan mic INMP441 - firmware chua dung mic), roi doi -1 thanh so chan duoi day.
// 2026-11-15: da thu bang day dupont, man chay on: CS -> GPIO39, RESET -> GPIO40, VCC -> 5V thang
// (CS noi GND / RESET noi EN / VCC qua cong tac tren mach in lam man trang). GPIO39/40 la chan mic
// INMP441 -> muon dung mic phai doi chan khac. -1 = cach noi cu (CS noi GND, RESET noi EN).
#ifndef TFT_CS_PIN
#define TFT_CS_PIN  39
#endif
#ifndef TFT_RST_PIN
#define TFT_RST_PIN 40
#endif
static constexpr int TFT_CS   = TFT_CS_PIN;
static constexpr int TFT_RST  = TFT_RST_PIN;
static constexpr int TFT_DC   = 48;   // DC / RS  (GPIO48 - chung LED RGB nen LED sang; 40 sau khi han lai)
static constexpr int TFT_MOSI = 2;    // SDI (MOSI) - chung duong voi LED do tren bo
static constexpr int TFT_SCLK = 1;    // SCK
#endif
static constexpr int TFT_BL   = -1;   // LED noi thang 3.3V, khong dieu khien bang GPIO
static constexpr int TFT_MISO = -1;   // SDO de trong (NC)

// 4 nut bam - xlsx dong 24-27. Nut noi GND, dung INPUT_PULLUP.
// Ten hang so theo chuc nang UI, comment ghi ten nut in tren mach.
static constexpr int BTN_VOLUME_DOWN = 3;   // DOWN1   - GPIO3  (strapping, chi doc sau khi boot)
static constexpr int BTN_VOLUME_UP   = 14;  // UP1     - GPIO14
static constexpr int BTN_NEXT        = 21;  // SELECT1 - GPIO21
static constexpr int BTN_POWER       = 47;  // BACK1   - GPIO47
static constexpr int BTN_MANUAL      = -1;  // Mach khong co nut thu 5; dung lenh serial 'm'

static constexpr bool TFT_BL_ACTIVE_HIGH = true;
static constexpr bool TFT_INVERT = false;
static constexpr bool TFT_RGB_ORDER = false;
// Day han tay + MOSI chung LED do nen 27 MHz khong on dinh (man trang). 10 MHz chay duoc.
// Thu tang dan 16 -> 20 -> 27 MHz de bot giat; lui ve muc truoc neu man nhieu/trang.
static constexpr int TFT_SPI_HZ = 20000000;  // do thuc te: 20 MHz ~97 ms/khung (nhanh nhat); 26/40 MHz bi chia xung lui ~146 ms