#pragma once
#include <Arduino.h>
#include <atomic>
static constexpr uint32_t HOLD_MS = 2000;
static constexpr size_t AUTO_FRAMES = 1;
struct Snapshot {
  String mode = "manual", view = "home", text = "", resultId = "", controlId = "";
  bool running = false, countAll = false;
  float hold = 0;
};
struct Action { char name[20]; bool hasCount = false, countAll = false; };
struct CapturedFrame { uint8_t *data = nullptr; size_t size = 0; uint32_t captured = 0; };
