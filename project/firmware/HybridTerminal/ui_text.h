#pragma once
// Chu tieng Viet co dau tren TFT: font VLW nhung trong flash (fonts_vi.h).
// Tach khoi .ino vi Arduino tu sinh prototype truoc khi khai bao enum.
#include <Arduino.h>
#include "hardware.h"
#include "fonts_vi.h"

// ===== FONT TIENG VIET (VLW tao boi scripts/make_tft_assets.py) =====
enum FontId { F_12B, F_13B, F_15, F_15B, F_17, F_24B, F_28B, F_56B, F_COUNT };
static const uint8_t* const FONT_DATA[F_COUNT] = {
  FONT_VI_12B, FONT_VI_13B, FONT_VI_15, FONT_VI_15B, FONT_VI_17, FONT_VI_24B, FONT_VI_28B, FONT_VI_56B,
};
struct ViFont {
  lgfx::v1::PointerWrapper data;
  lgfx::v1::VLWfont font;
  bool ok = false;
};
static ViFont viFonts[F_COUNT];

static void fontsBegin() {
  for (int i = 0; i < F_COUNT; i++) {
    viFonts[i].data.set(FONT_DATA[i]);
    viFonts[i].ok = viFonts[i].font.loadFont(&viFonts[i].data);
    if (!viFonts[i].ok) Serial.printf("[FONT] Khong nap duoc font %d\n", i);
  }
}

static void useFont(FontId id) {
  if (viFonts[id].ok) lcd.setFont(&viFonts[id].font);
  else lcd.setFont(&fonts::Font2);
  lcd.setTextSize(1);
}

// Ve chu theo duong chan chu (baseline). bg phai dung mau nen de khu rang cua.
static int text(FontId f, const String& s, int x, int baseline, uint16_t fg, uint16_t bg,
                textdatum_t datum = textdatum_t::baseline_left) {
  useFont(f);
  lcd.setTextDatum(datum);
  lcd.setTextColor(fg, bg);
  return lcd.drawString(s, x, baseline);
}

static int textWidth(FontId f, const String& s) {
  useFont(f);
  return lcd.textWidth(s);
}

// Xuong dong theo tu trong be rong maxW. Dong cuoi bi cat thi them "…".
static int wrapText(FontId f, const String& s, int x, int baseline, int maxW, int lineH,
                    int maxLines, uint16_t fg, uint16_t bg) {
  String line, word;
  int lines = 0;
  auto flush = [&]() {
    if (lines >= maxLines) return;
    text(f, line, x, baseline + lines * lineH, fg, bg);
    lines++;
  };
  for (size_t i = 0; i <= s.length(); i++) {
    char c = i < s.length() ? s[i] : ' ';
    if (c != ' ') { word += c; continue; }
    if (!word.length()) continue;
    String candidate = line.length() ? line + " " + word : word;
    if (line.length() && textWidth(f, candidate) > maxW) {
      if (lines == maxLines - 1) {  // het cho: cat dong cuoi
        text(f, line + "…", x, baseline + lines * lineH, fg, bg);
        return maxLines;
      }
      flush();
      line = word;
    } else {
      line = candidate;
    }
    word = "";
  }
  if (line.length()) flush();
  return lines;
}

