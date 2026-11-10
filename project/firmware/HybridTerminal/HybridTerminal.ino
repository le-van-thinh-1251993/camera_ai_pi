
#include <Arduino.h>
#include <atomic>
#include <math.h>

#include "config.h"
#include "hardware.h"
#include "live_camera.h"
#include "lan_client.h"
#include "ui_text.h"
#include "welcome_audio.h"

// =====================================================
// ESP32-S3 + TFT ILI9488 3.5" + OV3660 - NHAN DIEN CU CHI TAY
// Giao dien "Dem xanh - the ket qua" (mockup phuong an A).
//
//   Man chao  -> phat "Xin chao", SELECT de vao live
//   Live      -> khung camera 320x240 + cot ket qua / y nghia + footer
//                AUTO: giu yen tay 2 s -> tu chup.  MANUAL: SELECT de chup.
//   Ket qua   -> anh vua chup dung yen, loa doc y nghia, 8 s sau ve live
//
// BACK1   GPIO47 = doi che do AUTO / MANUAL
// SELECT1 GPIO21 = bat dau / chup / live tiep
// DOWN1   GPIO3  = tang am luong
// UP1     GPIO14 = giam am luong
//
// Stream cho dashboard: http://<ip>:81/stream
// Serial: b s + -  (gia lap 4 nut)   i = thong ke live
// Doi file "Xin chao": python scripts/make_tft_assets.py --wav <file.wav>
// =====================================================

// ===== NUT BAM =====
static const int BUTTON_PINS[4] = {BTN_POWER, BTN_NEXT, BTN_VOLUME_DOWN, BTN_VOLUME_UP};
static const char* BUTTON_NAMES[4] = {"BACK1", "SELECT1", "DOWN1", "UP1"};
enum Key { KEY_BACK = 0, KEY_SELECT = 1, KEY_VOL_UP = 2, KEY_VOL_DOWN = 3 };

struct ButtonState {
  bool raw = false, stable = false;
  uint32_t changedAt = 0;
};
static ButtonState buttons[4];

// ===== MAU (phuong an A) =====
static constexpr uint16_t rgb(uint32_t c) {
  return uint16_t(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}
static constexpr uint16_t C_BG = rgb(0x0b1626);
static constexpr uint16_t C_PANEL = rgb(0x12233a);
static constexpr uint16_t C_LINE = rgb(0x21385a);
static constexpr uint16_t C_FG = rgb(0xe8f0f8);
static constexpr uint16_t C_MUTE = rgb(0x8aa0b8);
static constexpr uint16_t C_ACC = rgb(0x38d6c9);
static constexpr uint16_t C_WARN = rgb(0xffc24b);
static constexpr uint16_t C_OK = rgb(0x4ade80);
static constexpr uint16_t C_RED = rgb(0xff5d5d);
static constexpr uint16_t C_BADGE = rgb(0x0b0f14);

// ===== BO CUC 480x320 =====
static constexpr int CAM_X = 8, CAM_Y = 8, CAM_W = 320, CAM_H = 240;
static constexpr int SIDE_X = 338, SIDE_W = 134;
static constexpr int CARD1_Y = 8, CARD1_H = 98;
static constexpr int CARD2_Y = 112, CARD2_H = 136;
static constexpr int CARD_PAD = 10;
static constexpr int FOOT_Y = 258, FOOT_H = 62;
static constexpr int WELCOME_FOOT_H = 52;

// ===== TRANG THAI =====
enum Screen { SCR_WELCOME, SCR_LIVE, SCR_BUSY, SCR_RESULT, SCR_ERROR };
static Screen screen = SCR_WELCOME;
static bool autoMode = true;
static bool cameraOK = false;
static uint32_t stageAt = 0;     // luc vao trang thai hien tai
static int lastTenths = -1;      // dem nguoc da ve (don vi 0,1 s)
static bool speaking = false;    // dang phat ket qua

static int volumePercent = AUDIO_GAIN_PERCENT;
static std::atomic<bool> cancelAudio{false};
static LanClient lan;

// Ket qua gan nhat
static String resultBig = "—";
static String resultMeaning;
static bool resultKnown = false;

static const char* HINT_AUTO = "Giữ yên tay 2 giây để tự chụp.";
static const char* HINT_MANUAL = "Giơ tay cố định vào khung rồi nhấn SELECT để chụp.";

// =====================================================
// THANH PHAN VE
// =====================================================

static void speakerIcon(int x, int cy, uint16_t color) {
  lcd.fillRect(x, cy - 3, 5, 7, color);
  lcd.fillTriangle(x + 4, cy, x + 10, cy - 7, x + 10, cy + 7, color);
  lcd.drawArc(x + 10, cy, 6, 5, 300, 60, color);
  lcd.drawArc(x + 10, cy, 10, 9, 300, 60, color);
}

// O phim tren footer: chip ten nut + nhan chuc nang. key "VOL" ve hai tam giac.
static void keyChip(int x, int cy, const char* key, const String& label, bool strong,
                    bool dim = false) {
  bool arrows = !strcmp(key, "VOL");
  int chipW = arrows ? 30 : textWidth(F_12B, key) + 12;
  bool filled = strong && !dim;
  uint16_t edge = dim ? C_LINE : C_ACC;
  if (filled) lcd.fillRoundRect(x, cy - 10, chipW, 20, 5, C_ACC);
  else lcd.drawRoundRect(x, cy - 10, chipW, 20, 5, edge);
  uint16_t chipFg = filled ? C_BG : edge;
  uint16_t chipBg = filled ? C_ACC : C_PANEL;
  if (arrows) {
    lcd.fillTriangle(x + 6, cy + 4, x + 14, cy + 4, x + 10, cy - 4, chipFg);   // len
    lcd.fillTriangle(x + 16, cy - 4, x + 24, cy - 4, x + 20, cy + 4, chipFg);  // xuong
  } else {
    text(F_12B, key, x + 6, cy + 4, chipFg, chipBg);
  }
  text(F_13B, label, x + chipW + 7, cy + 4, dim ? C_MUTE : C_FG, C_PANEL);
}

static String volumeLabel() { return "Âm lượng " + String(volumePercent) + "%"; }

static void drawFooter() {
  if (screen == SCR_WELCOME) {
    int y = 320 - WELCOME_FOOT_H;
    lcd.fillRect(0, y, 480, WELCOME_FOOT_H, C_PANEL);
    lcd.drawFastHLine(0, y, 480, C_LINE);
    keyChip(18, y + 26, "SELECT", "Bắt đầu", true);
    text(F_15, volumeLabel(), 462, y + 31, C_MUTE, C_PANEL, textdatum_t::baseline_right);
    return;
  }
  lcd.fillRect(0, FOOT_Y, 480, FOOT_H, C_PANEL);
  lcd.drawFastHLine(0, FOOT_Y, 480, C_LINE);
  int cy = FOOT_Y + FOOT_H / 2;
  bool busy = screen == SCR_BUSY;
  keyChip(10, cy, "BACK", autoMode ? "Chế độ: AUTO" : "Chế độ: MANUAL", true, busy);
  const char* sel = screen == SCR_RESULT || screen == SCR_ERROR ? "Live tiếp"
                    : busy                                      ? "Đang xử lý"
                    : autoMode                                  ? "Chụp ngay"
                                                                : "Chụp ảnh";
  keyChip(182, cy, "SELECT", sel, !autoMode && screen == SCR_LIVE, busy);
  keyChip(326, cy, "VOL", volumeLabel(), false);
}

static void card(int y, int h) {
  lcd.fillRoundRect(SIDE_X, y, SIDE_W, h, 8, C_PANEL);
  lcd.drawRoundRect(SIDE_X, y, SIDE_W, h, 8, C_LINE);
}

// The "Ket qua": nhan, chu to, dong phu (co the kem bieu tuong loa).
static void drawResultCard(const char* label, const String& big, uint16_t bigColor,
                           const String& sub, uint16_t subColor, bool icon = false) {
  card(CARD1_Y, CARD1_H);
  int x = SIDE_X + CARD_PAD, maxW = SIDE_W - 2 * CARD_PAD;
  text(F_12B, label, x, CARD1_Y + 20, C_MUTE, C_PANEL);
  FontId bigFont = textWidth(F_28B, big) <= maxW ? F_28B : F_15B;
  text(bigFont, big, x, CARD1_Y + 58, bigColor, C_PANEL);
  if (sub.length()) {
    int tx = x;
    if (icon) { speakerIcon(x, CARD1_Y + 81, subColor); tx += 22; }
    text(F_13B, sub, tx, CARD1_Y + 86, subColor, C_PANEL);
  }
}

static void drawMeaningCard(const String& meaning) {
  card(CARD2_Y, CARD2_H);
  int x = SIDE_X + CARD_PAD;
  text(F_12B, "Ý NGHĨA", x, CARD2_Y + 20, C_MUTE, C_PANEL);
  wrapText(F_15, meaning, x, CARD2_Y + 44, SIDE_W - 2 * CARD_PAD, 20, 5, C_FG, C_PANEL);
}

static void camOutline(uint16_t color) {
  lcd.drawRect(CAM_X - 3, CAM_Y - 3, CAM_W + 6, CAM_H + 6, color);
  lcd.drawRect(CAM_X - 2, CAM_Y - 2, CAM_W + 4, CAM_H + 4, color);
}

// Nhan "● LIVE" / "■ ĐÃ CHỤP" o goc anh (ve lai sau moi khung).
static void drawBadge(bool frozen) {
  const char* label = frozen ? "ĐÃ CHỤP" : "LIVE";
  uint16_t color = frozen ? C_OK : C_RED;
  int w = textWidth(F_12B, label) + 26;
  int x = CAM_X + 8, y = CAM_Y + 8;
  lcd.fillRoundRect(x, y, w, 20, 4, C_BADGE);
  if (frozen) lcd.fillRect(x + 7, y + 6, 8, 8, color);
  else lcd.fillCircle(x + 11, y + 10, 4, color);
  text(F_12B, label, x + 19, y + 15, color, C_BADGE);
}

static float countdownLeft() {
  int32_t left = (int32_t)AUTO_HOLD_MS - (int32_t)(millis() - stageAt);
  return left > 0 ? left / 1000.0f : 0.0f;
}

static String secondsText(float s) {
  char buf[12];
  snprintf(buf, sizeof buf, "%.1f", s);
  String out(buf);
  out.replace(".", ",");
  return out;
}

// Thanh tien do day khung anh + dong "Tu chup sau ..." (chi ve lai khi doi so).
static void drawCountdown() {
  float left = countdownLeft();
  float done = 1.0f - left / (AUTO_HOLD_MS / 1000.0f);
  int by = CAM_Y + CAM_H - 8;
  lcd.fillRect(CAM_X, by, CAM_W, 8, C_BADGE);
  lcd.fillRect(CAM_X, by, int(CAM_W * constrain(done, 0.0f, 1.0f)), 8, C_WARN);
  int tenths = int(ceilf(left * 10));
  if (tenths == lastTenths) return;
  lastTenths = tenths;
  int x = SIDE_X + CARD_PAD;
  lcd.fillRect(x, CARD1_Y + 68, SIDE_W - 2 * CARD_PAD, 24, C_PANEL);
  text(F_13B, "Tự chụp sau " + secondsText(left) + " s", x, CARD1_Y + 86, C_WARN, C_PANEL);
}

// =====================================================
// CAC MAN HINH
// =====================================================

static void drawWelcome(bool playing) {
  if (playing) {
    lcd.fillScreen(C_BG);
    text(F_12B, "NHẬN DIỆN CỬ CHỈ TAY", 36, 52, C_ACC, C_BG);
    text(F_56B, "Xin chào!", 34, 118, C_FG, C_BG);
    wrapText(F_17, "Giơ tay trước camera, máy sẽ đếm số ngón và đọc kết quả.", 36, 162, 400,
             24, 2, C_MUTE, C_BG);
    drawFooter();
  }
  lcd.fillRect(30, 200, 420, 32, C_BG);
  if (playing) {
    speakerIcon(36, 216, C_ACC);
    text(F_15B, "Đang phát lời chào…", 62, 222, C_ACC, C_BG);
  } else {
    text(F_15B, "Nhấn SELECT để bắt đầu.", 36, 222, C_ACC, C_BG);
  }
}

// Khung tinh cua man live (ve 1 lan khi vao live / doi che do).
static void drawLiveChrome() {
  lcd.fillScreen(C_BG);
  camOutline(C_LINE);
  if (autoMode) {
    drawResultCard("KẾT QUẢ", "—", C_MUTE, "", C_WARN);
    drawMeaningCard(HINT_AUTO);
  } else {
    drawResultCard(resultKnown ? "KẾT QUẢ TRƯỚC" : "KẾT QUẢ", resultBig,
                   resultKnown ? C_FG : C_MUTE, "Nhấn SELECT", C_ACC);
    drawMeaningCard(resultMeaning.length() ? resultMeaning : String(HINT_MANUAL));
  }
  drawFooter();
  if (!cameraOK) {
    lcd.fillRect(CAM_X, CAM_Y, CAM_W, CAM_H, C_PANEL);
    text(F_15B, "Không khởi động được camera", CAM_X + CAM_W / 2, CAM_Y + CAM_H / 2, C_WARN,
         C_PANEL, textdatum_t::baseline_center);
  }
}

static void startLive() {
  screen = SCR_LIVE;
  stageAt = millis();
  lastTenths = -1;
  drawLiveChrome();
}

static void showError(const String& message) {
  Serial.printf("[UI] LOI: %s\n", message.c_str());
  screen = SCR_ERROR;
  stageAt = millis();
  camOutline(C_WARN);
  drawResultCard("KẾT QUẢ", "Lỗi", C_WARN, "Thử lại sau 5 s", C_MUTE);
  drawMeaningCard(message);
  drawFooter();
}

static String nextRoundText() { return "Live tiếp sau " + String(AUTO_NEXT_ROUND_MS / 1000) + " s"; }

// =====================================================
// AM THANH
// =====================================================

static void speakerBeep() {
#if ENABLE_I2S_AUDIO
  const uint32_t rate = 16000, count = 1920;
  const size_t total = 44 + count * 2;
  uint8_t* wav = (uint8_t*)malloc(total);
  if (!wav) return;
  auto w16 = [&](int p, uint16_t v) { wav[p] = v & 0xFF; wav[p + 1] = v >> 8; };
  auto w32 = [&](int p, uint32_t v) {
    for (int i = 0; i < 4; i++) wav[p + i] = (v >> (i * 8)) & 0xFF;
  };
  memcpy(wav, "RIFF", 4); w32(4, total - 8); memcpy(wav + 8, "WAVEfmt ", 8);
  w32(16, 16); w16(20, 1); w16(22, 1); w32(24, rate); w32(28, rate * 2); w16(32, 2); w16(34, 16);
  memcpy(wav + 36, "data", 4); w32(40, count * 2);
  for (uint32_t i = 0; i < count; i++) {
    float env = fminf(1.0f, i / 160.0f) * fminf(1.0f, (count - 1 - i) / 160.0f);
    float v = sinf(2.0f * 3.14159265f * 880.0f * i / rate);
    w16(44 + i * 2, (uint16_t)(int16_t)(v * 8500.0f * env));
  }
  cancelAudio.store(false);
  playWav(wav, total, cancelAudio);
  free(wav);
#endif
}

// playWav() chinh am luong ngay trong bo dem -> chep file chao ra RAM truoc.
static void playWelcome() {
  uint8_t* wav = (uint8_t*)heap_caps_malloc(sizeof(WELCOME_WAV), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) return;
  memcpy_P(wav, WELCOME_WAV, sizeof(WELCOME_WAV));
  cancelAudio.store(false);
  bool ok = playWav(wav, sizeof(WELCOME_WAV), cancelAudio);
  Serial.printf("[AUDIO] Xin chao: %s\n", ok ? "OK" : "LOI");
  free(wav);
}

// =====================================================
// CHUP + NHAN DIEN
// =====================================================

static String newRequestId() {
  uint8_t b[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    memcpy(b + i, &r, 4);
  }
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  char out[37];
  snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
           b[14], b[15]);
  return String(out);
}

static void captureAndRecognize() {
  if (!cameraOK) { showError("Camera chưa sẵn sàng."); return; }
  Serial.printf("[CHUP] %s\n", autoMode ? "auto" : "manual");
  if (!liveGrabFrame(true)) { showError("Chụp ảnh lỗi, thử lại nhé."); return; }

  // Giu anh vua chup tren khung (dashboard cung thay anh nay qua stream)
  screen = SCR_BUSY;
  liveDrawFrame(CAM_X, CAM_Y, CAM_W, CAM_H);
  camOutline(C_OK);
  drawBadge(true);
  drawResultCard("KẾT QUẢ", "…", C_MUTE, "Đang nhận diện", C_ACC);
  drawMeaningCard("Đang gửi ảnh lên Pi.");
  drawFooter();
  speakerBeep();

  if (WiFi.status() != WL_CONNECTED) { showError("Chưa kết nối WiFi."); return; }

  String id = newRequestId();
  JsonDocument reply;
  uint32_t t0 = millis();
  if (!lan.recognize(DEVICE_ID, id, liveDrawBuf, liveDrawLen, autoMode, reply)) {
    showError(lan.lastStatus == -10 ? String("Không tìm thấy Pi trong mạng LAN.")
                                    : "Gửi ảnh lên Pi lỗi (mã " + String(lan.lastStatus) + ").");
    return;
  }

  JsonObject r = reply["result"];
  String gesture = r["gesture"] | "Unknown";
  int fingers = r["finger_count"] | -1;
  resultMeaning = r["text"] | "Tôi chưa nhận diện được cử chỉ.";
  resultKnown = gesture.startsWith("Count_") && fingers >= 0;
  resultBig = resultKnown ? String(fingers) + " ngón" : String("Chưa rõ");
  Serial.printf("[KET QUA] %s gesture=%s fingers=%d (%lums)\n", id.c_str(), gesture.c_str(),
                fingers, (unsigned long)(millis() - t0));

  // Man ket qua: anh dung yen + loa doc y nghia
  screen = SCR_RESULT;
  speaking = true;
  camOutline(C_OK);
  drawResultCard("KẾT QUẢ", resultBig, resultKnown ? C_FG : C_WARN, "Đang đọc", C_ACC, true);
  drawMeaningCard(resultMeaning);
  drawFooter();

  bool played = false;
  uint8_t* wav = (uint8_t*)heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t wavLen = 0;
  if (wav && lan.audio(DEVICE_ID, id, wav, MAX_WAV_BYTES, wavLen)) {
    cancelAudio.store(false);
    played = playWav(wav, wavLen, cancelAudio);
  }
  free(wav);
  speaking = false;
  drawResultCard("KẾT QUẢ", resultBig, resultKnown ? C_FG : C_WARN,
                 AUTO_NEXT_ROUND_MS ? nextRoundText() : String(""), C_MUTE);
  lan.ack(DEVICE_ID, id, true, played);
  stageAt = millis();  // tinh thoi gian giu ket qua tu luc doc xong
}

// Moi vong loop: lay khung moi nhat, ve, dem nguoc; het gio ket qua thi ve live.
static void liveTick() {
  if (screen == SCR_RESULT || screen == SCR_ERROR) {
    uint32_t wait = screen == SCR_ERROR ? 5000 : AUTO_NEXT_ROUND_MS;
    if (wait && millis() - stageAt >= wait) startLive();
    return;
  }
  if (screen != SCR_LIVE || !cameraOK) return;
  if (liveGrabFrame()) {
    liveDrawFrame(CAM_X, CAM_Y, CAM_W, CAM_H);
    drawBadge(false);
  }
  if (autoMode) {
    drawCountdown();
    if (countdownLeft() <= 0.0f) captureAndRecognize();
  }
}

// =====================================================
// NUT BAM
// =====================================================

static void changeVolume(int delta) {
  volumePercent = constrain(volumePercent + delta, 0, 100);
  currentVolumePercent.store(volumePercent);
  Serial.printf("[VOLUME] %d%%\n", volumePercent);
  drawFooter();
  speakerBeep();
}

static void handleButton(int key) {
  Serial.printf("[BUTTON] %s | screen=%d\n", BUTTON_NAMES[key], (int)screen);
  if (key == KEY_VOL_UP) { changeVolume(+10); return; }
  if (key == KEY_VOL_DOWN) { changeVolume(-10); return; }
  if (screen == SCR_BUSY) return;

  if (key == KEY_SELECT) {
    if (screen == SCR_LIVE) captureAndRecognize();  // AUTO: chup ngay, MANUAL: chup
    else startLive();                               // man chao, ket qua, loi
    return;
  }

  // BACK: doi AUTO / MANUAL
  if (screen == SCR_WELCOME) return;
  autoMode = !autoMode;
  Serial.printf("[MODE] %s\n", autoMode ? "AUTO" : "MANUAL");
  if (screen == SCR_LIVE) startLive();
  else drawFooter();
}

static void scanButtons() {
  for (int i = 0; i < 4; i++) {
    bool pressed = digitalRead(BUTTON_PINS[i]) == LOW;
    if (pressed != buttons[i].raw) {
      buttons[i].raw = pressed;
      buttons[i].changedAt = millis();
    }
    if (pressed != buttons[i].stable && millis() - buttons[i].changedAt >= DEBOUNCE_MS) {
      buttons[i].stable = pressed;
      if (pressed) handleButton(i);
    }
  }
}

static void serialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'b') handleButton(KEY_BACK);
    else if (c == 's') handleButton(KEY_SELECT);
    else if (c == '+') handleButton(KEY_VOL_UP);
    else if (c == '-') handleButton(KEY_VOL_DOWN);
    else if (c == 'i')
      Serial.printf("[LIVE] fps=%.1f frames=%lu dropped=%lu cam=%lums ve=%lums jpeg=%uB wifi=%s\n",
                    liveStats.fps, (unsigned long)liveStats.frames,
                    (unsigned long)liveStats.dropped, (unsigned long)liveStats.camMs,
                    (unsigned long)liveStats.tftMs, (unsigned)liveStats.lastBytes,
                    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-");
  }
}

// =====================================================
// SETUP / LOOP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\n=== ESP32-S3 NHAN DIEN CU CHI TAY ===");

#if SHARED_CAM_TFT_BUS
  cameraOK = liveCameraBegin();  // PCB goc: camera phai nhuong bus truoc lcd.init()
#endif
  if (!beginHardware() || !lcdReady) {
    Serial.println("[ERROR] Khoi tao TFT loi");
    return;
  }
#if !SHARED_CAM_TFT_BUS
  cameraOK = liveCameraBegin();
#endif
  Serial.printf("[CAM] %s\n", cameraOK ? "LIVE OK" : "KHOI TAO LOI");
  fontsBegin();
  liveWifiBegin();

  for (int i = 0; i < 4; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    bool pressed = digitalRead(BUTTON_PINS[i]) == LOW;
    buttons[i].raw = buttons[i].stable = pressed;
    buttons[i].changedAt = millis();
  }
  currentVolumePercent.store(volumePercent);

  screen = SCR_WELCOME;
  drawWelcome(true);
  playWelcome();
  drawWelcome(false);
  Serial.println("[READY] SELECT = vao live, BACK = AUTO/MANUAL, DOWN1/UP1 = am luong");
}

void loop() {
  if (!lcdReady) { delay(1000); return; }
  scanButtons();
  serialCommands();
  liveWifiTick();
  if (screen == SCR_WELCOME) {
#if !SHARED_CAM_TFT_BUS
    // Bo da sua: camera van chay de dashboard xem duoc ngay tu man chao.
    if (cameraOK) liveGrabFrame();
#endif
    delay(20);
    return;
  }
  liveTick();
  delay(1);
}
