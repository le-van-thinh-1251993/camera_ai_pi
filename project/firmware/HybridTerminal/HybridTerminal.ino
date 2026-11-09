
#include <Arduino.h>
#include <atomic>
#include <math.h>

#include "config.h"
#include "hardware.h"

// =====================================================
// ESP32-S3 + TFT ILI9488 3.5 INCH
// PCB BUTTON UI + I2S SPEAKER DEMO
//
// BACK1   = GPIO47 = Next option
// SELECT1 = GPIO21 = Confirm
// DOWN1   = GPIO3  = Volume UP
// UP1     = GPIO14 = Volume DOWN
//
// SW2 = Physical power switch
//
// Camera is disabled for this demo.
// =====================================================

#if !TFT_KIND
#error "Enable TFT_KIND = 1 in config.h"
#endif

#if ENABLE_CAMERA
#error "Set ENABLE_CAMERA = 0 in config.h"
#endif

// ===== REAL PCB GPIO =====

static const int PIN_BACK   = 47;
static const int PIN_SELECT = 21;
static const int PIN_DOWN   = 3;
static const int PIN_UP     = 14;

static const int BUTTON_PINS[4] = {
  PIN_BACK,
  PIN_SELECT,
  PIN_DOWN,
  PIN_UP
};

static const char* BUTTON_NAMES[4] = {
  "BACK1",
  "SELECT1",
  "DOWN1",
  "UP1"
};

// ===== DISPLAY COLORS =====

static const uint16_t BG     = 0x0841;
static const uint16_t PANEL  = 0x10A2;
static const uint16_t CYAN   = 0x07FF;
static const uint16_t WHITE  = 0xFFFF;
static const uint16_t GREEN  = 0x07E0;
static const uint16_t YELLOW = 0xFFE0;
static const uint16_t GRAY   = 0x8410;

// ===== UI STATES =====

enum Page {
  PAGE_WELCOME,
  PAGE_MENU,
  PAGE_PREVIEW,
  PAGE_RESULT
};

static Page currentPage = PAGE_WELCOME;

static int menuIndex = 0;
static int pageIndex = 0;
static int demoGesture = 0;

static int volumePercent = 60;

static bool needsRedraw = true;

static uint32_t volumeOverlayUntil = 0;

static std::atomic<bool> cancelAudio{false};

// ===== BUTTON DEBOUNCE =====

struct ButtonState {
  bool raw = false;
  bool stable = false;
  uint32_t changedAt = 0;
};

static ButtonState buttons[4];

static const uint32_t BUTTON_DEBOUNCE_MS = 35;

// =====================================================
// DRAW HELPERS
// =====================================================

static void drawText(
  int x,
  int y,
  const String& value,
  uint16_t color = WHITE,
  int size = 2
) {
  lcd.setTextColor(color, BG);
  lcd.setTextSize(size);
  lcd.setCursor(x, y);
  lcd.print(value);
}

static void drawPanel(
  int x,
  int y,
  int w,
  int h,
  uint16_t borderColor = GRAY
) {
  lcd.fillRoundRect(x, y, w, h, 9, PANEL);
  lcd.drawRoundRect(x, y, w, h, 9, borderColor);
}

static void drawHeader(
  const String& title,
  const String& status
) {
  lcd.fillScreen(BG);

  lcd.fillRect(
    0, 0,
    lcd.width(), 44,
    0x1084
  );

  drawText(12, 12, title, CYAN, 2);

  lcd.setTextSize(1);
  lcd.setTextColor(GREEN, 0x1084);

  lcd.setCursor(lcd.width() - 85, 19);
  lcd.print(status);

  lcd.drawFastHLine(
    0, 44,
    lcd.width(),
    CYAN
  );
}

static void drawFooter(const String& text) {
  int y = lcd.height() - 31;

  lcd.fillRect(
    0, y,
    lcd.width(), 31,
    0x1084
  );

  drawText(8, y + 10, text, WHITE, 1);
}

static void drawOption(
  int y,
  const String& text,
  bool selected
) {
  int width = lcd.width();

  drawPanel(
    18, y,
    width - 36, 46,
    selected ? CYAN : GRAY
  );

  if (selected) {
    lcd.fillRect(
      20, y + 2,
      6, 42,
      CYAN
    );
  }

  drawText(
    34, y + 14,
    text,
    selected ? YELLOW : WHITE,
    2
  );
}

// =====================================================
// VOLUME OVERLAY
// =====================================================

static void drawVolumeOverlay() {
  int y = lcd.height() - 65;

  lcd.fillRoundRect(
    12, y,
    lcd.width() - 24, 30,
    5, 0x2104
  );

  lcd.setTextSize(2);
  lcd.setTextColor(YELLOW, 0x2104);

  lcd.setCursor(22, y + 6);

  lcd.printf(
    "AM LUONG: %d%%",
    volumePercent
  );
}

// =====================================================
// MAIN UI RENDER
// =====================================================

static void renderScreen() {
  if (!lcdReady) return;

  int w = lcd.width();
  int h = lcd.height();

  switch (currentPage) {

    // ===== WELCOME =====

    case PAGE_WELCOME: {
      drawHeader(
        "GESTURE AI",
        "OFFLINE"
      );

      drawPanel(
        18, 63,
        w - 36,
        h - 117,
        CYAN
      );

      drawText(
        35, 86,
        "XIN CHAO THAY",
        YELLOW, 2
      );

      drawText(
        35, 128,
        "ESP32-S3 + TFT",
        WHITE, 2
      );

      drawText(
        35, 164,
        "Camera: CHUA KET NOI",
        GREEN, 1
      );

      drawOption(
        h - 112,
        "VAO MENU",
        true
      );

      drawFooter(
        "SELECT: VAO MENU"
      );

      break;
    }

    // ===== MAIN MENU =====

    case PAGE_MENU: {
      drawHeader(
        "MENU CHINH",
        "DEMO"
      );

      const char* items[3] = {
        "CAMERA PREVIEW",
        "KET QUA DEMO",
        "QUAY VE"
      };

      for (int i = 0; i < 3; i++) {
        drawOption(
          54 + i * 60,
          items[i],
          menuIndex == i
        );
      }

      drawFooter(
        "BACK: CHUYEN   SELECT: CHON"
      );

      break;
    }

    // ===== CAMERA PREVIEW =====

    case PAGE_PREVIEW: {
      drawHeader(
        "CAMERA PREVIEW",
        "OFFLINE"
      );

      int fx = 16;
      int fy = 55;
      int fw = w - 32;
      int fh = h - 208;

      // Outer camera frame
      lcd.drawRoundRect(
        fx, fy, fw, fh,
        7, CYAN
      );

      // Inner camera frame
      lcd.drawRoundRect(
        fx + 9,
        fy + 9,
        fw - 18,
        fh - 18,
        5, GRAY
      );

      // Center crosshair
      int cx = w / 2;
      int cy = fy + fh / 2;

      lcd.drawLine(
        cx - 22, cy,
        cx + 22, cy,
        GREEN
      );

      lcd.drawLine(
        cx, cy - 22,
        cx, cy + 22,
        GREEN
      );

      drawText(
        35, fy + 24,
        "CAMERA OFF",
        YELLOW, 2
      );

      drawText(
        35, fy + 55,
        "Khung preview gia lap",
        WHITE, 1
      );

      drawOption(
        h - 143,
        "XEM KET QUA",
        pageIndex == 0
      );

      drawOption(
        h - 91,
        "QUAY VE",
        pageIndex == 1
      );

      drawFooter(
        "BACK: CHUYEN   SELECT: CHON"
      );

      break;
    }

    // ===== DEMO RESULT =====

    case PAGE_RESULT: {
      drawHeader(
        "KET QUA DEMO",
        "MO PHONG"
      );

      drawPanel(
        18, 56,
        w - 36,
        h - 205,
        GREEN
      );

      drawText(
        35, 77,
        "CU CHI GIA LAP",
        WHITE, 2
      );

      const char* gestures[3] = {
        "OPEN PALM",
        "TWO",
        "FIST"
      };

      drawText(
        35, 117,
        gestures[demoGesture % 3],
        YELLOW, 3
      );

      drawText(
        35, 162,
        "Khong phai ket qua AI",
        GRAY, 1
      );

      drawOption(
        h - 143,
        "DOI CU CHI",
        pageIndex == 0
      );

      drawOption(
        h - 91,
        "QUAY VE",
        pageIndex == 1
      );

      drawFooter(
        "BACK: CHUYEN   SELECT: CHON"
      );

      break;
    }
  }

  if (
    volumeOverlayUntil != 0 &&
    (int32_t)(volumeOverlayUntil - millis()) > 0
  ) {
    drawVolumeOverlay();
  }

  needsRedraw = false;
}

// =====================================================
// I2S SPEAKER FEEDBACK
// Uses playWav() from existing hardware.h
// =====================================================

static void speakerBeep() {

#if ENABLE_I2S_AUDIO

  const uint32_t sampleRate = 16000;
  const uint32_t sampleCount = 1920;

  const size_t totalBytes =
    44 + sampleCount * 2;

  uint8_t* wav =
    (uint8_t*)malloc(totalBytes);

  if (!wav) {
    Serial.println(
      "[AUDIO] Not enough memory"
    );
    return;
  }

  memset(wav, 0, totalBytes);

  auto write16 = [&](int pos, uint16_t value) {
    wav[pos] = value & 0xFF;
    wav[pos + 1] = value >> 8;
  };

  auto write32 = [&](int pos, uint32_t value) {
    for (int i = 0; i < 4; i++) {
      wav[pos + i] =
        (value >> (i * 8)) & 0xFF;
    }
  };

  memcpy(wav, "RIFF", 4);
  write32(4, totalBytes - 8);

  memcpy(wav + 8, "WAVEfmt ", 8);

  write32(16, 16);
  write16(20, 1);
  write16(22, 1);

  write32(24, sampleRate);
  write32(28, sampleRate * 2);

  write16(32, 2);
  write16(34, 16);

  memcpy(wav + 36, "data", 4);
  write32(40, sampleCount * 2);

  for (uint32_t i = 0; i < sampleCount; i++) {

    float envelope =
      fminf(1.0f, float(i) / 160.0f) *
      fminf(
        1.0f,
        float(sampleCount - 1 - i) / 160.0f
      );

    float wave =
      sinf(
        2.0f * 3.14159265f *
        880.0f * i / sampleRate
      );

    int16_t sample =
      (int16_t)(wave * 8500.0f * envelope);

    write16(
      44 + i * 2,
      (uint16_t)sample
    );
  }

  cancelAudio.store(false);

  bool played =
    playWav(wav, totalBytes, cancelAudio);

  Serial.printf(
    "[AUDIO] Beep %s | Volume=%d%%\n",
    played ? "OK" : "FAILED",
    currentVolumePercent.load()
  );

  free(wav);

#else

  Serial.println(
    "[AUDIO] ENABLE_I2S_AUDIO = 0"
  );

#endif
}

// =====================================================
// BUTTON ACTIONS
// =====================================================

static void handleButton(int key) {

  Serial.printf(
    "[BUTTON] %s | PAGE=%d\n",
    BUTTON_NAMES[key],
    (int)currentPage
  );

  // ===== DOWN1: VOLUME UP =====

  if (key == 2) {

    volumePercent =
      constrain(volumePercent + 10, 0, 100);

    currentVolumePercent.store(
      volumePercent
    );

    volumeOverlayUntil = millis() + 1200;

    needsRedraw = true;

    Serial.printf(
      "[VOLUME] UP: %d%%\n",
      volumePercent
    );

    speakerBeep();

    return;
  }

  // ===== UP1: VOLUME DOWN =====

  if (key == 3) {

    volumePercent =
      constrain(volumePercent - 10, 0, 100);

    currentVolumePercent.store(
      volumePercent
    );

    volumeOverlayUntil = millis() + 1200;

    needsRedraw = true;

    Serial.printf(
      "[VOLUME] DOWN: %d%%\n",
      volumePercent
    );

    speakerBeep();

    return;
  }

  // ===== WELCOME =====

  if (currentPage == PAGE_WELCOME) {

    if (key == 1) {
      currentPage = PAGE_MENU;
      menuIndex = 0;
      needsRedraw = true;
    }

    return;
  }

  // ===== MAIN MENU =====

  if (currentPage == PAGE_MENU) {

    // BACK1 = Next option
    if (key == 0) {

      menuIndex =
        (menuIndex + 1) % 3;

      needsRedraw = true;

      return;
    }

    // SELECT1 = Confirm
    if (key == 1) {

      if (menuIndex == 0) {
        currentPage = PAGE_PREVIEW;
      }
      else if (menuIndex == 1) {
        currentPage = PAGE_RESULT;
      }
      else {
        currentPage = PAGE_WELCOME;
      }

      pageIndex = 0;
      needsRedraw = true;

      return;
    }
  }

  // ===== SUB-PAGES =====

  if (
    currentPage == PAGE_PREVIEW ||
    currentPage == PAGE_RESULT
  ) {

    // BACK1 cycles between options
    if (key == 0) {

      pageIndex =
        (pageIndex + 1) % 2;

      needsRedraw = true;

      return;
    }

    // SELECT1 confirms
    if (key == 1) {

      // QUAY VE
      if (pageIndex == 1) {

        currentPage = PAGE_MENU;
        pageIndex = 0;

        needsRedraw = true;

        return;
      }

      // Camera demo -> result demo
      if (currentPage == PAGE_PREVIEW) {

        currentPage = PAGE_RESULT;
        pageIndex = 0;

        needsRedraw = true;

        return;
      }

      // Change demo gesture
      if (currentPage == PAGE_RESULT) {

        demoGesture =
          (demoGesture + 1) % 3;

        needsRedraw = true;

        return;
      }
    }
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.println(
    "================================"
  );
  Serial.println(
    " ESP32-S3 PCB UI + I2S AUDIO"
  );
  Serial.println(
    "================================"
  );

  // Initialize TFT and I2S hardware
  bool hardwareOK = beginHardware();

  if (!hardwareOK || !lcdReady) {

    Serial.println(
      "[ERROR] TFT initialization failed"
    );

    return;
  }

  // Initialize four physical buttons
  for (int i = 0; i < 4; i++) {

    int pin = BUTTON_PINS[i];

    pinMode(pin, INPUT_PULLUP);

    bool pressed =
      digitalRead(pin) == LOW;

    buttons[i].raw = pressed;
    buttons[i].stable = pressed;
    buttons[i].changedAt = millis();

    Serial.printf(
      "[GPIO] %s = GPIO%d\n",
      BUTTON_NAMES[i],
      pin
    );
  }

  currentVolumePercent.store(
    volumePercent
  );

  currentPage = PAGE_WELCOME;
  needsRedraw = true;

  renderScreen();

  Serial.println(
    "[READY] TFT initialized"
  );

  Serial.println(
    "[READY] BACK1 GPIO47"
  );

  Serial.println(
    "[READY] SELECT1 GPIO21"
  );

  Serial.println(
    "[READY] DOWN1 GPIO3 VOL+"
  );

  Serial.println(
    "[READY] UP1 GPIO14 VOL-"
  );

  Serial.println(
    "[READY] Serial: b s + -"
  );
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  if (!lcdReady) {
    delay(1000);
    return;
  }

  // ===== PHYSICAL BUTTON SCAN =====

  for (int i = 0; i < 4; i++) {

    int pin = BUTTON_PINS[i];

    bool pressed =
      digitalRead(pin) == LOW;

    if (pressed != buttons[i].raw) {

      buttons[i].raw = pressed;
      buttons[i].changedAt = millis();
    }

    if (
      pressed != buttons[i].stable &&
      millis() - buttons[i].changedAt
        >= BUTTON_DEBOUNCE_MS
    ) {

      buttons[i].stable = pressed;

      Serial.printf(
        "GPIO%d %s %s\n",
        pin,
        BUTTON_NAMES[i],
        pressed ? "PRESSED" : "RELEASED"
      );

      if (pressed) {
        handleButton(i);
      }
    }
  }

  // ===== SERIAL TEST =====

  while (Serial.available()) {

    char command = Serial.read();

    if (command == 'b') {
      handleButton(0);
    }
    else if (command == 's') {
      handleButton(1);
    }
    else if (command == '+') {
      handleButton(2);
    }
    else if (command == '-') {
      handleButton(3);
    }
  }

  // ===== HIDE VOLUME OVERLAY =====

  if (
    volumeOverlayUntil != 0 &&
    (int32_t)(millis() - volumeOverlayUntil) >= 0
  ) {

    volumeOverlayUntil = 0;
    needsRedraw = true;
  }

  // ===== REFRESH TFT =====

  if (needsRedraw) {
    renderScreen();
  }

  delay(5);
}
