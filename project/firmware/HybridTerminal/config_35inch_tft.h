#pragma once

// 3.5-inch ILI9488 SPI TFT preset — GPIO from schematic Bang_Ket_Noi_Module.
// Confirmed: SCK=12, MOSI=11, CS=9, DC=10, RST=8.  LED tied to 3.3V (no GPIO).
#ifndef TFT_KIND
#define TFT_KIND 1  // ILI9488
#endif

static constexpr int TFT_NATIVE_WIDTH = 320;
static constexpr int TFT_NATIVE_HEIGHT = 480;
static constexpr int TFT_ROTATION = 3;

// SPI bus — schematic rows 3-9
static constexpr int TFT_SCLK = 12;   // GPIO12 — SCK
static constexpr int TFT_MOSI = 11;   // GPIO11 — SDI (MOSI)
static constexpr int TFT_MISO = -1;   // SDO left NC on schematic
static constexpr int TFT_CS   = 9;    // GPIO9  — CS
static constexpr int TFT_DC   = 10;   // GPIO10 — DC / RS
static constexpr int TFT_RST  = 8;    // GPIO8  — RESET
static constexpr int TFT_BL   = -1;   // LED tied to 3.3V rail, no GPIO control

// Four physical tact switches — schematic rows 24-27.
// Use the "Chân Nối Tới ESP32" column for the socket pin assignment.
// The "Tên Net" values in those rows are not in button-label order.
// Mapping for UI: BACK1→cycle, SELECT1→confirm, DOWN1→volume up, UP1→volume down.
static constexpr int BTN_POWER       = 47;  // BACK1   — GPIO47
static constexpr int BTN_NEXT        = 21;  // SELECT1 — GPIO21
static constexpr int BTN_VOLUME_UP   = 14;  // UP1     — GPIO14
static constexpr int BTN_VOLUME_DOWN = 3;   // DOWN1   — GPIO3
static constexpr int BTN_MANUAL      = -1;  // No 5th button on board; use serial 'm'

static constexpr bool TFT_BL_ACTIVE_HIGH = true;
static constexpr bool TFT_INVERT = false;
static constexpr bool TFT_RGB_ORDER = false;
static constexpr int TFT_SPI_HZ = 27000000;  // ILI9488 supports up to ~40 MHz; 27 MHz is safe
