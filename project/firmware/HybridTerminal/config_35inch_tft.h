#pragma once

// 3.5-inch SPI TFT preset. Fill the GPIO values after inspecting the board.
// Supported controllers: 1=ILI9488, 2=ST7796, 3=ILI9341.
#ifndef TFT_KIND
#define TFT_KIND 1
#endif

static constexpr int TFT_NATIVE_WIDTH = 320;
static constexpr int TFT_NATIVE_HEIGHT = 480;
static constexpr int TFT_ROTATION = 1;

static constexpr int TFT_SCLK = -1;
static constexpr int TFT_MOSI = -1;
static constexpr int TFT_MISO = -1;
static constexpr int TFT_CS = -1;
static constexpr int TFT_DC = -1;
static constexpr int TFT_RST = -1;
static constexpr int TFT_BL = -1;
static constexpr int BTN_NEXT = -1;
static constexpr int BTN_VOLUME_UP = -1;
static constexpr int BTN_VOLUME_DOWN = -1;
static constexpr int BTN_MANUAL = -1;
static constexpr int BTN_POWER = -1; // Soft standby input; actual power cut needs a latch circuit.

static constexpr bool TFT_BL_ACTIVE_HIGH = true;
static constexpr bool TFT_INVERT = false;
static constexpr bool TFT_RGB_ORDER = false;
static constexpr int TFT_SPI_HZ = 20000000;

// Common wiring example only; do not enable it without checking the ESP32 PCB.
// SCLK=12, MOSI=11, MISO=13, CS=10, DC=9, RST=8, BL=7.
