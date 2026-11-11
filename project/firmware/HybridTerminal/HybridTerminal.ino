
#include <Arduino.h>
#include <atomic>
#include <math.h>

#include "config.h"
#include "hardware.h"
#include "live_camera.h"
#include "wifi_setup.h"
#include "lan_client.h"
#include "device_sync.h"
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
// Cau hinh WiFi: http://<ip>/ hoac hotspot AI-Camera-XXXX -> http://192.168.4.1/
//   Giu BACK 3 s o man chao (hoac giu BACK khi cap dien) de mo hotspot doi WiFi.
// Serial: b s + -  (gia lap 4 nut)   i = thong ke live   w = mo hotspot WiFi
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
static constexpr uint16_t C_WARN_BG = rgb(0x3a2a07), C_WARN_TXT = rgb(0xe9d7ae);
static constexpr uint16_t C_OK_BG = rgb(0x0f3326), C_OK_TXT = rgb(0xbfe9cf);
static constexpr uint16_t C_WHITE = rgb(0xffffff);

// ===== BO CUC 480x320 =====
static constexpr int CAM_X = 8, CAM_Y = 8, CAM_W = 320, CAM_H = 240;
static constexpr int SIDE_X = 338, SIDE_W = 134;
static constexpr int CARD1_Y = 8, CARD1_H = 98;
static constexpr int CARD2_Y = 112, CARD2_H = 136;
static constexpr int CARD_PAD = 10;
static constexpr int FOOT_Y = 258, FOOT_H = 62;
static constexpr int WELCOME_FOOT_H = 52;

// ===== TRANG THAI =====
enum Screen { SCR_WELCOME, SCR_LIVE, SCR_BUSY, SCR_RESULT, SCR_ERROR, SCR_WIFI };
static Screen screen = SCR_WELCOME;
static WifiView wifiView = WV_LOST;  // enum o wifi_setup.h (tranh loi prototype cua Arduino)
static Screen screenBeforeWifi = SCR_WELCOME;
static uint32_t wifiViewAt = 0;
static constexpr uint32_t BACK_HOLD_MS = 3000;     // giu BACK de mo hotspot doi WiFi
static constexpr uint32_t WIFI_BACK_SHOW_MS = 8000; // man "Da ket noi lai"
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
  if (screen == SCR_WIFI) return;  // man WiFi co footer rieng
  if (screen == SCR_WELCOME) {
    int y = 320 - WELCOME_FOOT_H;
    lcd.fillRect(0, y, 480, WELCOME_FOOT_H, C_PANEL);
    lcd.drawFastHLine(0, y, 480, C_LINE);
    keyChip(18, y + 26, "SELECT", "Bắt đầu", true);
    keyChip(160, y + 26, "BACK", "Giữ 3 s: đổi WiFi", false);
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

// Cat chuoi UTF-8 cho vua be rong, them "…".
static String fitText(FontId f, String s, int maxW) {
  if (textWidth(f, s) <= maxW) return s;
  while (s.length() && textWidth(f, s + "…") > maxW) {
    int i = s.length() - 1;
    while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
    s.remove(i);
  }
  return s + "…";
}

// The mang tren man chao: IP de mo trinh duyet xem / doi WiFi.
static void drawNetCard() {
  const int x = 24, y = 176, w = 432, h = 82, tx = x + 14, rx = x + 262;
  lcd.fillRoundRect(x, y, w, h, 10, C_PANEL);
  lcd.drawRoundRect(x, y, w, h, 10, C_LINE);
  if (wifiState == WS_ONLINE && WiFi.status() == WL_CONNECTED) {
    text(F_12B, "MỞ TRÌNH DUYỆT ĐỂ XEM VÀ ĐỔI WIFI", tx, y + 20, C_MUTE, C_PANEL);
    text(F_28B, wifiIp, tx, y + 58, C_ACC, C_PANEL);
    const int maxW = x + w - rx - 26;
    lcd.fillCircle(rx + 4, y + 40, 4, C_OK);
    text(F_13B, fitText(F_13B, "WiFi " + String(wifiSsid), maxW), rx + 14, y + 45, C_FG, C_PANEL);
    // Trang thai may chu nhan dien (task theo doi cap nhat)
    uint16_t sc = serverState == SV_OK ? C_OK : serverState == SV_MISSING ? C_WARN : C_MUTE;
    String st = serverState == SV_OK      ? "Máy chủ " + IPAddress(serverIpRaw).toString()
                : serverState == SV_MISSING ? String("Chưa thấy máy chủ")
                                            : String("Đang tìm máy chủ…");
    lcd.fillCircle(rx + 4, y + 64, 4, sc);
    text(F_13B, fitText(F_13B, st, maxW), rx + 14, y + 69, sc == C_MUTE ? C_MUTE : sc, C_PANEL);
  } else {
    text(F_12B, "WIFI", tx, y + 20, C_MUTE, C_PANEL);
    String s = wifiSsid[0] ? "Đang kết nối \"" + String(wifiSsid) + "\"…" : String("Đang kết nối WiFi…");
    text(F_15B, fitText(F_15B, s, w - 28), tx, y + 48, C_WARN, C_PANEL);
    text(F_13B, "Giữ BACK 3 giây để chọn WiFi khác.", tx, y + 70, C_MUTE, C_PANEL);
  }
}

static void drawWelcome(bool full) {
  if (full) {
    lcd.fillScreen(C_BG);
    text(F_12B, "NHẬN DIỆN CỬ CHỈ TAY", 36, 44, C_ACC, C_BG);
    text(F_56B, "Xin chào!", 34, 106, C_FG, C_BG);
    wrapText(F_17, "Giơ tay trước camera, máy sẽ đếm số ngón và đọc kết quả.", 36, 140, 410,
             22, 2, C_MUTE, C_BG);
    drawFooter();
  }
  drawNetCard();
}

// Khung tinh cua man live (ve 1 lan khi vao live / doi che do).
static void drawLiveChrome() {
  lcd.fillScreen(C_BG);
  camOutline(C_LINE);
  if (serverState != SV_OK) {
    // Chi xem camera: chua gui anh duoc
    drawResultCard("KẾT QUẢ", "—", C_MUTE, "Chưa có máy chủ", C_WARN);
    drawMeaningCard(wifiState == WS_ONLINE ? "Chỉ xem camera. Nhấn SELECT để xem cách kết nối máy chủ."
                                           : "Chỉ xem camera. Đang chờ WiFi.");
  } else if (autoMode) {
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
// MAN WIFI: mat mang / hotspot cau hinh / da ket noi lai
// =====================================================

static void wifiTopBar(uint16_t dot, const String& title) {
  lcd.fillRect(0, 0, 480, 30, C_BG);
  lcd.drawFastHLine(0, 30, 480, C_LINE);
  lcd.fillCircle(18, 15, 4, dot);
  text(F_13B, fitText(F_13B, title, 340), 30, 20, C_FG, C_BG);
  text(F_12B, DEVICE_ID, 466, 20, C_MUTE, C_BG, textdatum_t::baseline_right);
}

static void wifiFooter(const String& sel, const String& right) {
  lcd.fillRect(0, 280, 480, 40, C_PANEL);
  lcd.drawFastHLine(0, 280, 480, C_LINE);
  keyChip(14, 300, "SELECT", sel, true);
  text(F_13B, right, 466, 305, C_MUTE, C_PANEL, textdatum_t::baseline_right);
}

static void wifiCard(int x, int y, int w, int h) {
  lcd.fillRoundRect(x, y, w, h, 10, C_PANEL);
  lcd.drawRoundRect(x, y, w, h, 10, C_LINE);
}

// Chu bi gach ngang (IP cu khong con dung).
static void struckText(FontId f, const String& s, int x, int baseline, uint16_t line) {
  text(f, s, x, baseline, C_MUTE, C_PANEL);
  lcd.fillRect(x, baseline - 6, textWidth(f, s), 2, line);
}

static String savedCountText() { return "Đã lưu " + String(wifiNetCount) + " mạng"; }

static int lostLastSec = -1;

static void drawLostCountdown() {
  int32_t left = (int32_t)WIFI_LOST_TO_AP_MS - (int32_t)(millis() - wifiStateAt);
  int sec = left > 0 ? (left + 999) / 1000 : 0;
  if (sec == lostLastSec) return;
  lostLastSec = sec;
  const int x = 30, y = 106;
  lcd.fillRect(x, y + 34, 182, 64, C_PANEL);
  text(F_28B, String(sec) + " giây", x, y + 62, C_FG, C_PANEL);
  text(F_13B, "nữa sẽ bật hotspot", x, y + 82, C_MUTE, C_PANEL);
  int done = 182 * (WIFI_LOST_TO_AP_MS / 1000 - sec) / (WIFI_LOST_TO_AP_MS / 1000);
  lcd.fillRoundRect(x, y + 92, 182, 6, 3, C_LINE);
  if (done > 0) lcd.fillRoundRect(x, y + 92, done, 6, 3, C_WARN);
}

static void drawWifiLost() {
  lcd.fillScreen(C_BG);
  wifiTopBar(C_RED, "Mất kết nối");
  lcd.fillRect(0, 31, 480, 62, C_WARN_BG);
  lcd.fillRect(0, 93, 480, 2, C_WARN);
  lcd.fillTriangle(35, 42, 17, 78, 53, 78, C_WARN);
  lcd.fillRect(33, 54, 4, 13, C_WARN_BG);
  lcd.fillRect(33, 70, 4, 4, C_WARN_BG);
  text(F_28B, fitText(F_28B, "Mất WiFi \"" + String(wifiSsid) + "\"", 400), 66, 66, C_WARN, C_WARN_BG);
  text(F_13B, "Nhận diện tạm dừng. Đang tự kết nối lại…", 66, 86, C_WARN_TXT, C_WARN_BG);

  wifiCard(18, 106, 206, 166);
  text(F_12B, "ĐANG THỬ LẠI", 30, 128, C_MUTE, C_PANEL);
  text(F_12B, "IP CŨ", 30, 236, C_MUTE, C_PANEL);
  struckText(F_15B, wifiOldIp[0] ? String(wifiOldIp) : String("—"), 30, 260, C_RED);

  wifiCard(236, 106, 226, 166);
  const int x = 248;
  text(F_12B, "CẤU HÌNH LẠI NGAY", x, 128, C_MUTE, C_PANEL);
  text(F_13B, "Nhấn SELECT để bật hotspot,", x, 152, C_FG, C_PANEL);
  text(F_13B, "nối điện thoại vào WiFi", x, 172, C_FG, C_PANEL);
  text(F_15B, apSsid, x, 196, C_FG, C_PANEL);
  text(F_13B, "rồi mở", x, 218, C_MUTE, C_PANEL);
  text(F_28B, "192.168.4.1", x, 254, C_ACC, C_PANEL);

  wifiFooter("Bật hotspot ngay", savedCountText());
  lostLastSec = -1;
  drawLostCountdown();
}

static String portalLine, portalFoot;

// Dong trang thai duoi man hotspot + footer: ve lai khi noi dung doi.
static void drawPortalStatus() {
  bool up = WiFi.status() == WL_CONNECTED;
  int phones = WiFi.softAPgetStationNum();
  String line;
  uint16_t color = C_MUTE;
  if (up) { line = "Hoặc trong mạng hiện tại: http://" + String(wifiIp) + "/"; color = C_ACC; }
  else if (wifiAttempting) { line = "Đang thử kết nối \"" + String(wifiSsid) + "\"…"; color = C_WARN; }
  else if (wifiFail[0]) { line = "\"" + String(wifiFail) + "\": " + String(wifiFailMsg); color = C_RED; }
  else if (phones > 0) { line = "Điện thoại đã vào hotspot. Đang chờ chọn WiFi…"; color = C_OK; }
  else line = "Đang chờ điện thoại kết nối hotspot…";
  String sel = up ? "Thoát" : wifiNetCount ? "Thử lại WiFi đã lưu" : "Chờ cấu hình";
  String foot = sel + "|" + String(phones);
  if (line != portalLine) {
    portalLine = line;
    lcd.fillRect(0, 240, 480, 38, C_BG);
    text(F_13B, fitText(F_13B, line, 452), 16, 264, color, C_BG);
  }
  if (foot != portalFoot) {
    portalFoot = foot;
    wifiFooter(sel, "Hotspot: " + String(phones) + " máy");
  }
}

static void portalStep(int n, int y) {
  lcd.fillCircle(208, y - 5, 11, C_ACC);
  text(F_13B, String(n), 208, y, C_BG, C_ACC, textdatum_t::baseline_center);
}

static void drawWifiPortal() {
  lcd.fillScreen(C_BG);
  const char* title = portalReason == PR_LOST     ? "Mất WiFi · hotspot đang bật"
                      : portalReason == PR_MANUAL ? "Đổi WiFi"
                                                  : "Chưa có WiFi · cần cấu hình";
  wifiTopBar(C_WARN, title);

  // QR chuan "WIFI:" - camera dien thoai quet la vao hotspot.
  lcd.fillRoundRect(16, 40, 160, 160, 8, C_WHITE);
  String qr = "WIFI:T:nopass;S:" + String(apSsid) + ";;";
  lcd.qrcode(qr.c_str(), 22, 46, 148, 3);
  text(F_12B, "Quét để vào hotspot", 96, 220, C_MUTE, C_BG, textdatum_t::baseline_center);

  const int x = 228, w = 236;
  portalStep(1, 60);
  text(F_13B, "Nối điện thoại vào WiFi", x, 60, C_FG, C_BG);
  text(F_15B, apSsid, x, 82, C_FG, C_BG);
  text(F_12B, "Không cần mật khẩu", x, 100, C_MUTE, C_BG);
  portalStep(2, 128);
  wrapText(F_13B, "Trang cấu hình tự mở. Nếu không, vào", x, 128, w, 18, 2, C_FG, C_BG);
  text(F_28B, "192.168.4.1", x, 178, C_ACC, C_BG);
  portalStep(3, 206);
  wrapText(F_13B, "Chọn WiFi, nhập mật khẩu rồi bấm Lưu.", x, 206, w, 18, 2, C_FG, C_BG);

  portalLine = portalFoot = "";
  drawPortalStatus();
}

static void drawWifiBack() {
  lcd.fillScreen(C_BG);
  wifiTopBar(C_OK, "WiFi: " + String(wifiSsid));
  bool hadOld = wifiOldIp[0] != 0, same = hadOld && !strcmp(wifiOldIp, wifiIp);
  lcd.fillRect(0, 31, 480, 62, C_OK_BG);
  lcd.fillRect(0, 93, 480, 2, C_OK);
  lcd.drawWideLine(22, 62, 32, 72, 4, C_OK);
  lcd.drawWideLine(32, 72, 50, 50, 4, C_OK);
  text(F_28B, hadOld ? "Đã kết nối lại" : "Đã kết nối WiFi", 66, 66, C_OK, C_OK_BG);
  text(F_13B, same ? "Địa chỉ không đổi."
              : hadOld ? "Địa chỉ đã đổi. Dùng địa chỉ mới bên dưới."
                       : "Mở địa chỉ bên dưới trên trình duyệt.",
       66, 86, C_OK_TXT, C_OK_BG);

  wifiCard(18, 106, 280, 166);
  text(F_12B, hadOld && !same ? "ĐỊA CHỈ MỚI" : "ĐỊA CHỈ", 30, 128, C_MUTE, C_PANEL);
  text(F_28B, wifiIp, 30, 168, C_ACC, C_PANEL);
  text(F_15B, String(DEVICE_ID) + ".local", 30, 204, C_FG, C_PANEL);
  text(F_12B, "Tên này không đổi khi đổi mạng.", 30, 226, C_MUTE, C_PANEL);

  wifiCard(310, 106, 152, 166);
  text(F_12B, "IP CŨ", 322, 128, C_MUTE, C_PANEL);
  if (hadOld && !same) struckText(F_15B, wifiOldIp, 322, 154, C_RED);
  else text(F_15B, "—", 322, 154, C_MUTE, C_PANEL);
  text(F_12B, "HOTSPOT", 322, 196, C_MUTE, C_PANEL);
  text(F_13B, apOn ? "Tắt sau ít giây" : "Đã tắt", 322, 220, C_MUTE, C_PANEL);

  wifiFooter("Vào ngay", "Tự quay lại sau " + String(WIFI_BACK_SHOW_MS / 1000) + " s");
}

// ----- Khong thay may chu nhan dien (khac WiFi / server chua chay) -----
static bool serverDismissed = false;  // nguoi dung chon "chi xem camera"
static uint32_t noServerChecks = 0;

static void drawNoServerStatus() {
  if (serverChecks == noServerChecks) return;
  noServerChecks = serverChecks;
  lcd.fillRect(0, 256, 480, 23, C_BG);
  String s = "Đã thử: " + String(serverTried[0] ? serverTried : "—") + " · tự thử lại mỗi 3 s";
  text(F_12B, fitText(F_12B, s, 452), 16, 272, C_MUTE, C_BG);
}

static void drawWifiNoServer() {
  lcd.fillScreen(C_BG);
  wifiTopBar(C_WARN, "Chưa kết nối máy chủ nhận diện");
  lcd.fillRect(0, 31, 480, 62, C_WARN_BG);
  lcd.fillRect(0, 93, 480, 2, C_WARN);
  lcd.fillTriangle(35, 42, 17, 78, 53, 78, C_WARN);
  lcd.fillRect(33, 54, 4, 13, C_WARN_BG);
  lcd.fillRect(33, 70, 4, 4, C_WARN_BG);
  text(F_28B, "Không thấy máy chủ", 66, 66, C_WARN, C_WARN_BG);
  text(F_13B, "Camera vẫn chạy, nhưng chưa gửi ảnh đi nhận diện được.", 66, 86, C_WARN_TXT, C_WARN_BG);

  wifiCard(18, 106, 206, 146);
  text(F_12B, "THIẾT BỊ ĐANG Ở", 30, 128, C_MUTE, C_PANEL);
  text(F_12B, "WiFi", 30, 148, C_MUTE, C_PANEL);
  text(F_15B, fitText(F_15B, wifiSsid, 182), 30, 168, C_FG, C_PANEL);
  text(F_12B, "IP", 30, 194, C_MUTE, C_PANEL);
  text(F_15B, wifiIp, 30, 214, C_ACC, C_PANEL);

  wifiCard(236, 106, 226, 146);
  const int x = 248, w = 204;
  text(F_12B, "CÁCH SỬA", x, 128, C_MUTE, C_PANEL);
  wrapText(F_13B, "1. Cho máy tính/Pi chạy server vào cùng WiFi \"" + String(wifiSsid) + "\".", x, 150,
           w, 18, 3, C_FG, C_PANEL);
  text(F_13B, "2. Hoặc nhập IP máy chủ tại", x, 210, C_FG, C_PANEL);
  text(F_13B, fitText(F_13B, "http://" + String(wifiIp) + "/", w), x, 230, C_ACC, C_PANEL);

  wifiFooter("Thử lại ngay", "BACK: chỉ xem camera");
  noServerChecks = serverChecks + 1;  // ep ve dong trang thai
  drawNoServerStatus();
}

static void enterWifi(WifiView v) {
  if (screen != SCR_WIFI) screenBeforeWifi = screen;
  screen = SCR_WIFI;
  wifiView = v;
  wifiViewAt = millis();
  Serial.printf("[UI] Man WiFi %d\n", (int)v);
  if (v == WV_LOST) drawWifiLost();
  else if (v == WV_PORTAL) drawWifiPortal();
  else if (v == WV_NOSERVER) drawWifiNoServer();
  else drawWifiBack();
}

static void leaveWifi() {
  if (screenBeforeWifi == SCR_WELCOME) {
    screen = SCR_WELCOME;
    drawWelcome(true);
  } else {
    startLive();
  }
}

// Goi moi vong loop: chuyen man theo trang thai WiFi, cap nhat dem nguoc.
static void wifiUiTick() {
  if (wifiChanged && screen != SCR_BUSY) {
    wifiChanged = false;
    switch (wifiState) {
      case WS_CONNECTING:
        if (screen == SCR_WELCOME) drawNetCard();
        break;
      case WS_ONLINE:
        lan.forgetHost();          // kiem tra may chu ngay tren mang moi
        serverDismissed = false;   // mang moi: bao lai neu van khong thay may chu
        if (screen == SCR_WIFI) {
          if (wifiView != WV_BACK && wifiView != WV_NOSERVER) enterWifi(WV_BACK);
        } else if (screen == SCR_WELCOME) drawNetCard();
        break;
      case WS_LOST:
        if (screen != SCR_WIFI || wifiView != WV_LOST) enterWifi(WV_LOST);
        break;
      case WS_PORTAL:
        if (screen != SCR_WIFI || wifiView != WV_PORTAL) enterWifi(WV_PORTAL);
        break;
    }
  }
  // May chu nhan dien: man cảnh bao khi khong thay, tu quay lai khi thay.
  if (serverState == SV_OK) serverDismissed = false;
  if (screen == SCR_WIFI && wifiView == WV_NOSERVER &&
      (serverState == SV_OK || wifiState != WS_ONLINE)) {
    if (serverState == SV_OK) { Serial.println("[LAN] Thay may chu -> ve giao dien chinh"); leaveWifi(); }
  } else if (wifiState == WS_ONLINE && serverState == SV_MISSING && !serverDismissed &&
             (screen == SCR_WELCOME || screen == SCR_LIVE || screen == SCR_ERROR)) {
    enterWifi(WV_NOSERVER);
  }
  if (serverChanged && screen != SCR_BUSY && screen != SCR_RESULT) {
    serverChanged = false;
    if (screen == SCR_WELCOME) drawNetCard();
    else if (screen == SCR_LIVE) startLive();  // ve lai the "chua co may chu" / bat lai dem nguoc
  }

  if (screen != SCR_WIFI) return;
  if (wifiView == WV_LOST) drawLostCountdown();
  else if (wifiView == WV_PORTAL) drawPortalStatus();
  else if (wifiView == WV_NOSERVER) drawNoServerStatus();
  else if (millis() - wifiViewAt >= WIFI_BACK_SHOW_MS) leaveWifi();
}

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

// ---------- Dong bo trang thai voi web (device_sync.h) ----------
static String lastResultId;

static void devPublish() {
  devState.autoMode = autoMode;
  devState.running = screen == SCR_LIVE || screen == SCR_BUSY;
  devState.hasFrame = cameraOK;
  devState.playing = speaking;
  devState.view = screen == SCR_WELCOME ? 0 : screen == SCR_LIVE ? 1 : screen == SCR_BUSY ? 2
                : screen == SCR_RESULT  ? 3 : screen == SCR_ERROR ? 4 : 5;
  devState.hold = screen == SCR_LIVE && autoMode && serverState == SV_OK
                      ? AUTO_HOLD_MS / 1000.0f - countdownLeft() : 0.0f;
  devState.volume = volumePercent;
}

// Tai am thanh ket qua tu may chu va doc tren loa.
static bool playResultAudio(const String& id) {
  bool played = false;
  uint8_t* wav = (uint8_t*)heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t wavLen = 0;
  if (wav && lan.audio(DEVICE_ID, id, wav, MAX_WAV_BYTES, wavLen)) {
    cancelAudio.store(false);
    played = playWav(wav, wavLen, cancelAudio);
  }
  free(wav);
  return played;
}

static void replayResult() {
  if (!lastResultId.length()) { speakerBeep(); return; }
  Serial.println("[LOA] Doc lai ket qua");
  speaking = true;
  devPublish();
  playResultAudio(lastResultId);
  speaking = false;
  devPublish();
}

static void captureAndRecognize() {
  if (!cameraOK) { showError("Camera chưa sẵn sàng."); return; }
  if (WiFi.status() != WL_CONNECTED) { showError("Chưa kết nối WiFi."); return; }
  if (serverState != SV_OK) {  // khong gui anh mu: mo man huong dan, khong chan nut
    enterWifi(WV_NOSERVER);
    return;
  }
  Serial.printf("[CHUP] %s\n", autoMode ? "auto" : "manual");
  if (!liveGrabFrame(true)) { showError("Chụp ảnh lỗi, thử lại nhé."); return; }

  // Giu anh vua chup tren khung (dashboard cung thay anh nay qua stream)
  screen = SCR_BUSY;
  devPublish();
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
    int st = lan.lastStatus;
    String cfg = " Đặt IP máy chủ tại http://" + String(wifiIp) + "/";
    showError(st == -10             ? "Không tìm thấy máy chủ." + cfg
              : st < 0              ? "Không kết nối được máy chủ cổng 8000." + cfg
              : st == 401 || st == 403 ? String("Sai LAN_API_KEY (khác với máy chủ).")
              : st == 503           ? String("Máy chủ chưa đặt LAN_API_KEY.")
                                    : "Máy chủ báo lỗi (mã " + String(st) + ").");
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

  lastResultId = id;
  strlcpy(devState.resultId, id.c_str(), sizeof devState.resultId);
  devPublish();
  bool played = playResultAudio(id);
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
  if (autoMode && serverState == SV_OK) {  // chua co may chu: khong tu chup
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

  if (screen == SCR_WIFI) {
    if (wifiView == WV_NOSERVER) {
      if (key == KEY_SELECT) { serverCheckNow = true; Serial.println("[LAN] Thu lai may chu"); }
      else { serverDismissed = true; startLive(); }  // BACK: chi xem camera
      return;
    }
    if (key != KEY_SELECT) return;
    if (wifiView == WV_LOST) wifiOpenPortal(PR_LOST);          // bat hotspot ngay
    else if (wifiView == WV_BACK) leaveWifi();                  // vao ngay
    else if (WiFi.status() == WL_CONNECTED) { leaveWifi(); wifiClosePortal(); }
    else if (wifiNetCount) wifiRetryNow();                      // thu lai mang da luu
    return;
  }

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

// Lenh tu dashboard (device_sync.h) - ap dung nhu bam nut tren bo.
static void applyWebCommand(const String& a) {
  Serial.printf("[WEB] Lenh %s | screen=%d\n", a.c_str(), (int)screen);
  if (a == "auto" || a == "manual") {
    bool want = a == "auto";
    if (autoMode != want) {
      autoMode = want;
      Serial.printf("[MODE] %s (tu web)\n", autoMode ? "AUTO" : "MANUAL");
      if (screen == SCR_LIVE) startLive();
      else drawFooter();
    }
    return;
  }
  if (a == "replay") { replayResult(); return; }
  if (a == "greeting") { speaking = true; devPublish(); playWelcome(); speaking = false; return; }
  if (screen == SCR_WIFI) return;  // dang o man WiFi / canh bao: bo qua lenh dieu huong
  if (a == "start") { if (screen != SCR_LIVE) startLive(); return; }
  if (a == "stop" || a == "home") { screen = SCR_WELCOME; drawWelcome(true); return; }
  if (a == "capture") {
    if (screen != SCR_LIVE) startLive();
    captureAndRecognize();
  }
}

static void devSyncTick() {
  devPublish();
  if (!devCmdPending || screen == SCR_BUSY) return;
  String action = devCmdAction;
  strlcpy(devState.controlId, devCmdId, sizeof devState.controlId);  // bao web: da nhan lenh
  devCmdPending = false;
  applyWebCommand(action);
  devPublish();
  devSyncNow = true;  // bao web ngay: nut tren web khong phai cho
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
  // Giu BACK 3 s o man chao -> mo hotspot doi WiFi.
  static bool backHoldFired = buttons[KEY_BACK].stable;  // dang giu tu luc cap dien
  const ButtonState& back = buttons[KEY_BACK];
  if (!back.stable) backHoldFired = false;
  else if (!backHoldFired && screen == SCR_WELCOME && millis() - back.changedAt >= BACK_HOLD_MS) {
    backHoldFired = true;
    Serial.println("[WIFI] Giu BACK -> mo hotspot");
    wifiOpenPortal(PR_MANUAL);
  }
}

static void serialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'b') handleButton(KEY_BACK);
    else if (c == 's') handleButton(KEY_SELECT);
    else if (c == '+') handleButton(KEY_VOL_UP);
    else if (c == '-') handleButton(KEY_VOL_DOWN);
    else if (c == 'w') wifiOpenPortal(PR_MANUAL);
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
  // Giu SCK o muc thap cang som cang tot: SPI mode 0 nghi o muc thap, tranh xung nhieu
  // lam man (CS noi GND) dem lech bit. Chi giam rui ro - sua dut diem xem config_35inch_tft.h.
  if (TFT_SCLK >= 0) { pinMode(TFT_SCLK, OUTPUT); digitalWrite(TFT_SCLK, LOW); }
  // Tat LED RGB WS2812 tren bo (GPIO48) - choi mat; gui 2 lan cho chac.
  // Neu GPIO48 dang lam chan TFT (DC/MOSI/SCK) thi KHONG dung toi: LED sang theo tin hieu
  // man hinh, phan mem khong tat duoc -> phai thao LED/dien tro hoac doi chan TFT.
  if (BOARD_RGB_PIN >= 0 && BOARD_RGB_PIN != TFT_DC && BOARD_RGB_PIN != TFT_MOSI &&
      BOARD_RGB_PIN != TFT_SCLK) {
    rgbLedWrite(BOARD_RGB_PIN, 0, 0, 0);
    delay(2);
    rgbLedWrite(BOARD_RGB_PIN, 0, 0, 0);
  }
  Serial.begin(115200);
  delay(250);
  Serial.println("\n=== ESP32-S3 NHAN DIEN CU CHI TAY ===");

#if SHARED_CAM_TFT_BUS
  cameraOK = liveCameraBegin();  // PCB goc: camera phai nhuong bus truoc lcd.init()
#endif
  Serial.printf("[TFT] ILI9488 SCK=%d MOSI=%d DC=%d CS=%d RST=%d shared_bus=%d\n",
                TFT_SCLK, TFT_MOSI, TFT_DC, TFT_CS, TFT_RST, (int)SHARED_CAM_TFT_BUS);
  if (!beginHardware() || !lcdReady) {
    Serial.println("[ERROR] Khoi tao TFT loi");
    return;
  }
  Serial.println("[TFT] init OK");
#if !SHARED_CAM_TFT_BUS
  cameraOK = liveCameraBegin();
#endif
  Serial.printf("[CAM] %s\n", cameraOK ? "LIVE OK" : "KHOI TAO LOI");
  fontsBegin();

  for (int i = 0; i < 4; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    bool pressed = digitalRead(BUTTON_PINS[i]) == LOW;
    buttons[i].raw = buttons[i].stable = pressed;
    buttons[i].changedAt = millis();
  }
  currentVolumePercent.store(volumePercent);

  // Giu BACK khi cap dien -> vao thang che do cau hinh WiFi.
  bool forcePortal = buttons[KEY_BACK].stable;
  if (forcePortal) Serial.println("[WIFI] BACK dang giu luc khoi dong -> hotspot cau hinh");
  wifiBegin(forcePortal);
  serverMonitorBegin();  // task rieng: kiem tra may chu nhan dien, khong chan giao dien
  devSyncBegin();        // task rieng: dong bo trang thai / lenh voi dashboard

  screen = SCR_WELCOME;
  drawWelcome(true);
  playWelcome();  // WiFi van ket noi ngam trong luc phat; loop() cap nhat the IP sau
  Serial.println("[READY] SELECT = vao live, BACK = AUTO/MANUAL (giu 3 s o man chao = doi WiFi), DOWN1/UP1 = am luong");
}

void loop() {
  if (!lcdReady) { delay(1000); return; }
  scanButtons();
  serialCommands();
  if (liveRestartReq) {  // /cam?spi=: khoi dong lai khi SPI ranh (khong cat ngang byte -> man khong lech)
    Serial.println("[TFT] Doi toc do SPI -> khoi dong lai");
    lcd.waitDisplay();
    delay(300);
    ESP.restart();
  }
  wifiTick();
  wifiUiTick();
  devSyncTick();
  if (screen == SCR_WIFI) {
#if !SHARED_CAM_TFT_BUS
    if (cameraOK) liveGrabFrame();  // dashboard van xem live trong luc o man WiFi / canh bao
#endif
    delay(10);
    return;
  }
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
