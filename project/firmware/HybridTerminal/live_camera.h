#pragma once
// =====================================================================
// LIVE CAMERA (TFT + MJPEG cho dashboard)
//
// SHARED_CAM_TFT_BUS 0 (bo da sua): TFT co day rieng, camera chay lien tuc,
// moi vong loop lay khung moi nhat ve len TFT va phat ra stream.
//
// SHARED_CAM_TFT_BUS 1 (PCB goc) - phan con lai cua chu thich nay:
// Tren PCB hien tai, camera Y2..Y6 va TFT MOSI/CS/RST/DC/SCK nam tren cung
// GPIO 11/9/8/10/12. Hai ben KHONG duoc phat tin hieu cung luc, nen moi khung
// hinh chay theo chu ky:
//
//   1. ESP32 nha 5 chan (input)        -> bat chan xuat cua cam bien (SCCB)
//   2. Cho 1 khung JPEG moi             -> chep sang bo dem rieng
//   3. Tat chan xuat cua cam bien       -> ESP32 noi lai 5 chan vao SPI
//   4. Khoi phuc TFT (lenh rac luc chup) -> ve anh len man hinh
//
// Thu tu bat/tat chan dam bao khong co luc nao 2 ben cung day 1 day.
// Cung khung hinh do duoc phat ra MJPEG http://<ip>:81/stream cho dashboard.
//
// Neu doc thanh ghi pad cua cam bien khong duoc, firmware chuyen sang che do
// an toan: init/deinit camera moi khung (cham hon nhung khong tranh chap bus).
// =====================================================================
#include <Arduino.h>
#include <algorithm>
#include <WiFi.h>
#include <driver/spi_master.h>
#include <ESPmDNS.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#include <soc/spi_periph.h>
#include <freertos/semphr.h>
#include "config.h"
#include <Preferences.h>

#if !ENABLE_CAMERA || !TFT_KIND
#error "live_camera.h can ENABLE_CAMERA=1 va TFT_KIND!=0"
#endif

// OV3660 / OV5640: thanh ghi bat tat chan xuat DVP.
// 0x3017: FREX, VSYNC, HREF, PCLK, D9..D6   0x3018: D5..D0
static constexpr int OV_PAD_OEN1 = 0x3017;
static constexpr int OV_PAD_OEN2 = 0x3018;

enum LiveRestoreMode : uint8_t {
  RESTORE_FAST = 0,  // gui lai lenh co ban, cho 5 ms
  RESTORE_SAFE = 1,  // nhu tren nhung cho 120 ms (khi TFT bi reset that)
  RESTORE_FULL = 2,  // lcd.init() lai toan bo
};

struct LiveStats {
  uint32_t frames = 0, dropped = 0, lastFrameMs = 0;
  float fps = 0;
  uint32_t camMs = 0, tftMs = 0;  // thoi gian moi pha, khung gan nhat
  size_t lastBytes = 0;
};

static bool liveReady = false;
static bool livePadGating = false;  // true = tat/bat chan cam bien qua SCCB
static uint8_t livePadOn1 = 0xFF, livePadOn2 = 0xFF;
static LiveRestoreMode liveRestoreMode = RESTORE_FAST;
static LiveStats liveStats;
// Kich thuoc live hien tai (doi duoc qua /cam?size=...). Nho hon = TFT ve nhanh hon, bot giat.
static framesize_t liveFrameSize = LIVE_FRAME_SIZE;
// Giu camera khi chup anh VGA / doi kich thuoc (task lay khung tam dung).
static SemaphoreHandle_t liveCamLock = nullptr;

// Bo dem ve (chi loop dung) va bo dem stream (co mutex, HTTP doc).
static uint8_t* liveDrawBuf = nullptr;
static size_t liveDrawLen = 0;
static uint8_t* liveStreamBuf = nullptr;
static size_t liveStreamLen = 0;
static volatile uint32_t liveStreamSeq = 0;
static SemaphoreHandle_t liveStreamLock = nullptr;
static constexpr size_t LIVE_MAX_JPEG = 80000;

static camera_config_t liveCameraConfig() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = CAM_XCLK_HZ;
  c.pixel_format = PIXFORMAT_JPEG;
  // Khoi tao o kich thuoc chup lon nhat de bo dem du cho anh VGA;
  // live doi xuong QVGA ngay sau init.
  c.frame_size = CAPTURE_FRAME_SIZE;
  c.jpeg_quality = LIVE_JPEG_QUALITY;
  c.fb_count = psramFound() ? 2 : 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  return c;
}

// Anh live sang + net: gain toi da CAM_GAINCEILING, bu phoi sang, tang
// sac net, giam nhieu, sua diem anh loi, bu sang ong kinh. Ham nao driver khong co thi bo qua.
#define CAM_SET(fn, v) do { if (s->fn) s->fn(s, v); } while (0)
static void liveTuneSensor(sensor_t* s) {
  CAM_SET(set_vflip, CAM_VFLIP);
  CAM_SET(set_hmirror, CAM_HMIRROR);
  CAM_SET(set_quality, LIVE_JPEG_QUALITY);
  CAM_SET(set_gain_ctrl, 1);
  CAM_SET(set_exposure_ctrl, 1);
  CAM_SET(set_aec2, 0);            // tat che do dem: tranh tut fps (giat) khi toi
  CAM_SET(set_gainceiling, CAM_GAINCEILING);
  CAM_SET(set_ae_level, CAM_AE_LEVEL);
  CAM_SET(set_brightness, CAM_BRIGHTNESS);
  CAM_SET(set_sharpness, CAM_SHARPNESS);
  CAM_SET(set_denoise, CAM_DENOISE);
  CAM_SET(set_bpc, 1);
  CAM_SET(set_wpc, 1);
  CAM_SET(set_lenc, 1);
  CAM_SET(set_whitebal, 1);
  CAM_SET(set_awb_gain, 1);
}
#undef CAM_SET

#if SHARED_CAM_TFT_BUS
// Chan dung chung: {GPIO, bit du lieu camera}
struct SharedPin { int gpio; int camBit; };
static constexpr SharedPin SHARED_PINS[] = {
  {Y2_GPIO_NUM, 0}, {Y3_GPIO_NUM, 1}, {Y4_GPIO_NUM, 2},
  {Y5_GPIO_NUM, 3}, {Y6_GPIO_NUM, 4},
};
static_assert(Y2_GPIO_NUM == TFT_MOSI && Y3_GPIO_NUM == TFT_CS && Y4_GPIO_NUM == TFT_RST &&
              Y5_GPIO_NUM == TFT_DC && Y6_GPIO_NUM == TFT_SCLK,
              "Bang chan dung chung khong khop config - kiem tra lai config.h");

// ---------- Dinh tuyen 5 chan dung chung ----------

// ESP32 nha chan: chi doc (camera). Phai goi TRUOC khi bat chan cam bien.
static void busPinsToCamera() {
  for (auto p : SHARED_PINS) {
    esp_rom_gpio_pad_select_gpio(p.gpio);
    gpio_set_direction((gpio_num_t)p.gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)p.gpio, GPIO_FLOATING);
#ifdef CAM_DATA_IN0_IDX
    esp_rom_gpio_connect_in_signal(p.gpio, CAM_DATA_IN0_IDX + p.camBit, false);
#endif
  }
}

// ESP32 lay lai chan cho TFT. Phai goi SAU khi da tat chan cam bien.
static void busPinsToTft() {
  // Muc nghi truoc khi bat output de tranh xung gai.
  gpio_set_level((gpio_num_t)TFT_CS, 1);
  gpio_set_level((gpio_num_t)TFT_RST, 1);
  for (auto p : SHARED_PINS) {
    esp_rom_gpio_pad_select_gpio(p.gpio);
    // INPUT_OUTPUT: van giu duong vao camera, nhung luc nay VSYNC bi keo thap
    // nen camera khong chup gi.
    gpio_set_direction((gpio_num_t)p.gpio, GPIO_MODE_INPUT_OUTPUT);
  }
  // gpio_set_direction() noi chan ve GPIO thuong; noi lai SCK/MOSI vao SPI2.
  esp_rom_gpio_connect_out_signal(TFT_SCLK, spi_periph_signal[SPI2_HOST].spiclk_out, false, false);
  esp_rom_gpio_connect_out_signal(TFT_MOSI, spi_periph_signal[SPI2_HOST].spid_out, false, false);
}

// ---------- Chan xuat cam bien ----------

static bool sensorPads(bool on) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;
  int a = s->set_reg(s, OV_PAD_OEN1, 0xFF, on ? livePadOn1 : 0x00);
  int b = s->set_reg(s, OV_PAD_OEN2, 0xFF, on ? livePadOn2 : 0x00);
  return a == 0 && b == 0;
}

// Soft reset (SYSTEM_CTROL0 0x3008 bit7): thanh ghi ve mac dinh luc cap dien,
// trong do chan xuat DVP bi tat. Dung truoc khi deinit o che do cham.
static void sensorSoftResetToIdle() {
  sensor_t* s = esp_camera_sensor_get();
  if (s) { s->set_reg(s, 0x3008, 0xFF, 0x82); delay(5); }
}

// Giu VSYNC/HREF/PCLK o muc thap khi cam bien tha noi -> driver khong chup rac.
static void holdSyncLinesLow() {
  gpio_pulldown_en((gpio_num_t)VSYNC_GPIO_NUM);
  gpio_pulldown_en((gpio_num_t)HREF_GPIO_NUM);
  gpio_pulldown_en((gpio_num_t)PCLK_GPIO_NUM);
}

// ---------- Khoi phuc TFT sau pha camera ----------

static void tftRestore() {
  if (liveRestoreMode == RESTORE_FULL) {
    // Reset cung + toan bo lenh init cua panel, khong khoi tao lai SPI bus.
    lcd.getPanel()->init(true);
    lcd.setRotation(TFT_ROTATION);
    lcd.startWrite(); lcd.setWindow(0, 0, 0, 0); lcd.endWrite();
    return;
  }
  // Luc chup, TFT co the da nhan lenh rac hoac bi reset (RST = Y4).
  delay(5);  // cho phep sau reset truoc khi gui lenh
  lcd.startWrite();
  lcd.writeCommand(0x11);  // SLPOUT
  lcd.endWrite();
  delay(liveRestoreMode == RESTORE_SAFE ? 120 : 5);
  lcd.startWrite();
  lcd.writeCommand(0x13);                      // NORON
  lcd.writeCommand(TFT_INVERT ? 0x21 : 0x20);  // INVON / INVOFF
  lcd.writeCommand(0x38);                      // IDMOFF
  lcd.writeCommand(0x3A); lcd.writeData(0x66); // 18-bit qua SPI
  lcd.writeCommand(0x29);                      // DISPON
  lcd.endWrite();
  // Ghi lai MADCTL va xoa bo nho cua so trong LovyanGFX.
  lcd.setRotation(TFT_ROTATION);
  lcd.startWrite(); lcd.setWindow(0, 0, 0, 0); lcd.endWrite();
}

#endif  // SHARED_CAM_TFT_BUS

// ---------- Stream MJPEG cho dashboard ----------

static httpd_handle_t liveStreamServer = nullptr, liveCtrlServer = nullptr;
// wifi_setup.h gan trang cau hinh WiFi (/, /api/wifi...) vao cung server cong 80.
static void (*liveRegisterExtra)(httpd_handle_t) = nullptr;

static esp_err_t liveStreamHandler(httpd_req_t* req) {
  static constexpr char BOUNDARY[] = "\r\n--frame\r\n";
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  uint8_t* local = (uint8_t*)heap_caps_malloc(LIVE_MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!local) local = (uint8_t*)malloc(LIVE_MAX_JPEG);
  if (!local) return httpd_resp_send_500(req);
  uint32_t seen = 0;
  esp_err_t err = ESP_OK;
  while (err == ESP_OK) {
    uint32_t waited = 0;
    while (liveStreamSeq == seen && waited < 5000) { vTaskDelay(pdMS_TO_TICKS(10)); waited += 10; }
    if (liveStreamSeq == seen) continue;  // chua co khung moi, giu ket noi
    size_t len = 0;
    if (xSemaphoreTake(liveStreamLock, pdMS_TO_TICKS(200)) == pdTRUE) {
      len = liveStreamLen; seen = liveStreamSeq;
      if (len) memcpy(local, liveStreamBuf, len);
      xSemaphoreGive(liveStreamLock);
    }
    if (!len) continue;
    char head[96];
    int n = snprintf(head, sizeof head, "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
    err = httpd_resp_send_chunk(req, BOUNDARY, sizeof(BOUNDARY) - 1);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, head, n);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char*)local, len);
  }
  free(local);
  return err;
}

static esp_err_t liveCaptureHandler(httpd_req_t* req) {
  uint8_t* local = nullptr; size_t len = 0;
  if (xSemaphoreTake(liveStreamLock, pdMS_TO_TICKS(500)) == pdTRUE) {
    len = liveStreamLen;
    if (len) { local = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); if (local) memcpy(local, liveStreamBuf, len); }
    xSemaphoreGive(liveStreamLock);
  }
  if (!local) { httpd_resp_set_status(req, "503 Service Unavailable"); return httpd_resp_send(req, "no frame", HTTPD_RESP_USE_STRLEN); }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t err = httpd_resp_send(req, (const char*)local, len);
  free(local);
  return err;
}

static esp_err_t liveStatusHandler(httpd_req_t* req) {
  char body[256];
  snprintf(body, sizeof body,
           "{\"device\":\"%s\",\"camera\":%s,\"pad_gating\":%s,\"fps\":%.1f,\"frames\":%lu,"
           "\"dropped\":%lu,\"cam_ms\":%lu,\"tft_ms\":%lu,\"restore\":%d}",
           DEVICE_ID, liveReady ? "true" : "false", livePadGating ? "true" : "false", liveStats.fps,
           (unsigned long)liveStats.frames, (unsigned long)liveStats.dropped,
           (unsigned long)liveStats.camMs, (unsigned long)liveStats.tftMs, (int)liveRestoreMode);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// GET /cam?ae=2&bright=1&gain=4&sharp=2&denoise=3&quality=8&contrast=1&sat=0
// Chinh cam bien ngay khi dang chay (khong can nap lai), tra ve gia tri hien tai.
// gain = muc tran gain 0..6 (2x,4x,8x,16x,32x,64x,128x). Mat sau khi khoi dong lai:
// gia tri tot thi ghi vao config.h.
// /cam?spi=<MHz>: luu toc do SPI cua man, loop() khoi dong lai ESP32 khi SPI ranh.
static volatile bool liveRestartReq = false;

static uint32_t tftSpiMhz() {
  Preferences p;
  p.begin("tft", true);
  uint32_t mhz = p.getUInt("spi_mhz", 0);
  p.end();
  return mhz ? mhz : TFT_SPI_HZ / 1000000;
}

static esp_err_t liveCamHandler(httpd_req_t* req) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) { httpd_resp_set_status(req, "503 Service Unavailable"); return httpd_resp_send(req, "no camera", HTTPD_RESP_USE_STRLEN); }
  char q[160] = "", v[12];
  httpd_req_get_url_query_str(req, q, sizeof q);
  auto val = [&](const char* k, int lo, int hi, int& out) {
    if (httpd_query_key_value(q, k, v, sizeof v) != ESP_OK) return false;
    out = constrain(atoi(v), lo, hi);
    return true;
  };
  int x;
  if (val("ae", -2, 2, x) && s->set_ae_level) s->set_ae_level(s, x);
  if (val("bright", -2, 2, x) && s->set_brightness) s->set_brightness(s, x);
  if (val("contrast", -2, 2, x) && s->set_contrast) s->set_contrast(s, x);
  if (val("sat", -2, 2, x) && s->set_saturation) s->set_saturation(s, x);
  if (val("sharp", -2, 2, x) && s->set_sharpness) s->set_sharpness(s, x);
  if (val("denoise", 0, 8, x) && s->set_denoise) s->set_denoise(s, x);
  if (val("quality", 4, 40, x) && s->set_quality) s->set_quality(s, x);
  if (val("gain", 0, 6, x) && s->set_gainceiling) s->set_gainceiling(s, (gainceiling_t)x);
  if (val("aec2", 0, 1, x) && s->set_aec2) s->set_aec2(s, x);
  if (val("spi", 0, 40, x)) {  // 0 = ve mac dinh TFT_SPI_HZ
    Preferences p;
    p.begin("tft", false);
    if (x) p.putUInt("spi_mhz", (uint32_t)max(x, 4)); else p.remove("spi_mhz");
    p.end();
    liveRestartReq = true;
  }
  if (httpd_query_key_value(q, "size", v, sizeof v) == ESP_OK) {
    framesize_t f = !strcmp(v, "qvga") ? FRAMESIZE_QVGA : !strcmp(v, "hqvga") ? FRAMESIZE_HQVGA
                  : !strcmp(v, "qcif") ? FRAMESIZE_QCIF : liveFrameSize;
    if (f != liveFrameSize) {
      bool locked = liveCamLock && xSemaphoreTake(liveCamLock, pdMS_TO_TICKS(2000)) == pdTRUE;
      liveFrameSize = f;
      s->set_framesize(s, f);
      if (locked) xSemaphoreGive(liveCamLock);
    }
  }
  char body[300];
  snprintf(body, sizeof body,
           "{\"ae\":%d,\"bright\":%d,\"contrast\":%d,\"sat\":%d,\"sharp\":%d,\"denoise\":%d,"
           "\"quality\":%d,\"gain\":%d,\"aec2\":%d,\"size\":\"%s\",\"fps\":%.1f,"
           "\"spi_mhz\":%lu,\"restarting\":%s}",
           s->status.ae_level, s->status.brightness, s->status.contrast, s->status.saturation,
           s->status.sharpness, s->status.denoise, s->status.quality, (int)s->status.gainceiling,
           s->status.aec2,
           liveFrameSize == FRAMESIZE_QVGA ? "qvga" : liveFrameSize == FRAMESIZE_HQVGA ? "hqvga" : "qcif",
           liveStats.fps, (unsigned long)tftSpiMhz(), liveRestartReq ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static void liveStartHttp() {
  if (liveCtrlServer) return;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = LIVE_HTTP_PORT; cfg.ctrl_port = 32768;
  cfg.max_uri_handlers = 12;
  cfg.lru_purge_enable = true;  // dien thoai mo nhieu ket noi khi do captive portal
  if (httpd_start(&liveCtrlServer, &cfg) == ESP_OK) {
    httpd_uri_t cap = {"/capture", HTTP_GET, liveCaptureHandler, nullptr};
    httpd_uri_t st = {"/status", HTTP_GET, liveStatusHandler, nullptr};
    httpd_register_uri_handler(liveCtrlServer, &cap);
    httpd_register_uri_handler(liveCtrlServer, &st);
    httpd_uri_t cam = {"/cam", HTTP_GET, liveCamHandler, nullptr};
    httpd_register_uri_handler(liveCtrlServer, &cam);
    if (liveRegisterExtra) liveRegisterExtra(liveCtrlServer);
  }
  // Stream chan luong rieng (giong CameraWebServer) de /status van tra loi.
  cfg.server_port = LIVE_HTTP_PORT + 1; cfg.ctrl_port = 32769;
  cfg.max_uri_handlers = 4;
  cfg.lru_purge_enable = false;
  if (httpd_start(&liveStreamServer, &cfg) == ESP_OK) {
    httpd_uri_t s = {"/stream", HTTP_GET, liveStreamHandler, nullptr};
    httpd_register_uri_handler(liveStreamServer, &s);
  }
  Serial.printf("[LIVE] http://%s:%u/stream  (http://%s.local:%u/stream)\n",
                WiFi.localIP().toString().c_str(), LIVE_HTTP_PORT + 1, DEVICE_ID, LIVE_HTTP_PORT + 1);
}

// Ket noi WiFi, mDNS va hotspot cau hinh: xem wifi_setup.h.

// Khung moi trong liveDrawBuf: chep sang stream va cap nhat thong ke.
static void liveCountFrame(size_t len) {
  uint32_t now = millis();
  if (liveStats.lastFrameMs) {
    float inst = 1000.0f / float(std::max<uint32_t>(1, now - liveStats.lastFrameMs));
    liveStats.fps = liveStats.fps ? liveStats.fps * 0.8f + inst * 0.2f : inst;
  }
  liveStats.lastFrameMs = now;
  liveStats.frames++;
  liveStats.lastBytes = len;
}

static void livePublishFrame(bool ok) {
  if (!ok) { liveStats.dropped++; return; }
  if (xSemaphoreTake(liveStreamLock, pdMS_TO_TICKS(20)) == pdTRUE) {
    memcpy(liveStreamBuf, liveDrawBuf, liveDrawLen);
    liveStreamLen = liveDrawLen;
    liveStreamSeq = liveStreamSeq + 1;
    xSemaphoreGive(liveStreamLock);
  }
  uint32_t now = millis();
  if (liveStats.lastFrameMs) {
    float inst = 1000.0f / float(std::max<uint32_t>(1, now - liveStats.lastFrameMs));
    liveStats.fps = liveStats.fps ? liveStats.fps * 0.8f + inst * 0.2f : inst;
  }
  liveStats.lastFrameMs = now;
  liveStats.frames++;
  liveStats.lastBytes = liveDrawLen;
}

static bool liveDrawHiRes = false;  // khung trong liveDrawBuf la VGA?

static bool liveAllocBuffers() {
  liveStreamLock = xSemaphoreCreateMutex();
  liveDrawBuf = (uint8_t*)heap_caps_malloc(LIVE_MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  liveStreamBuf = (uint8_t*)heap_caps_malloc(LIVE_MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!psramFound() || !liveDrawBuf || !liveStreamBuf || !liveStreamLock) {
    Serial.println("[CAM] Can PSRAM (Tools > PSRAM: OPI PSRAM).");
    return false;
  }
  return true;
}

static bool isFreshJpeg(camera_fb_t* fb, int64_t since, bool hiRes) {
  int64_t ts = int64_t(fb->timestamp.tv_sec) * 1000000LL + fb->timestamp.tv_usec;
  bool sizeOk = hiRes ? fb->width > 320 : fb->width <= 320;
  return ts >= since && sizeOk && fb->len > 4 && fb->len <= LIVE_MAX_JPEG &&
         fb->buf[0] == 0xFF && fb->buf[1] == 0xD8;
}

#if !SHARED_CAM_TFT_BUS
// ---------- Bo da sua: camera chay lien tuc ----------
// Task rieng lay khung camera lien tuc va dua thang len stream web: web chay theo toc do
// camera, khong bi keo cham boi viec ve TFT (~190 ms/khung o SPI 10 MHz).
// TFT (loop) chi lay khung moi nhat tu stream moi khi ve xong khung truoc.

static volatile uint32_t liveDrawnSeq = 0;  // khung stream TFT da ve

static void liveCamTask(void*) {
  for (;;) {
    if (xSemaphoreTake(liveCamLock, pdMS_TO_TICKS(100)) != pdTRUE) continue;  // dang chup VGA
    camera_fb_t* fb = esp_camera_fb_get();
    bool ok = false;
    size_t len = 0;
    if (fb) {
      len = fb->len;
      if (len > 4 && len <= LIVE_MAX_JPEG && fb->width <= 320 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
          xSemaphoreTake(liveStreamLock, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(liveStreamBuf, fb->buf, len);
        liveStreamLen = len;
        liveStreamSeq = liveStreamSeq + 1;
        xSemaphoreGive(liveStreamLock);
        ok = true;
      }
      esp_camera_fb_return(fb);
    }
    xSemaphoreGive(liveCamLock);
    if (ok) liveCountFrame(len);
    else liveStats.dropped++;
    vTaskDelay(1);
  }
}

static bool liveCameraBegin() {
  if (!liveAllocBuffers()) return false;
  camera_config_t c = liveCameraConfig();
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[CAM] esp_camera_init loi 0x%x\n", err);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  liveTuneSensor(s);
  s->set_framesize(s, liveFrameSize);
  Serial.printf("[CAM] Sensor PID=0x%04x - TFT bus rieng, chay dong thoi\n", s->id.PID);
  liveCamLock = xSemaphoreCreateMutex();
  liveReady = liveCamLock != nullptr;
  if (liveReady) xTaskCreatePinnedToCore(liveCamTask, "camgrab", 4096, nullptr, 2, nullptr, 0);
  return liveReady;
}

// hiRes=false: lay khung live moi nhat (task da chup) de ve TFT - tra false neu chua co khung moi.
// hiRes=true: tam dung task, chup VGA 640x480 de nhan dien.
static bool liveGrabFrame(bool hiRes = false) {
  if (!liveReady) return false;
  if (!hiRes) {
    if (liveStreamSeq == liveDrawnSeq) return false;
    if (xSemaphoreTake(liveStreamLock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    memcpy(liveDrawBuf, liveStreamBuf, liveStreamLen);
    liveDrawLen = liveStreamLen;
    liveDrawnSeq = liveStreamSeq;
    xSemaphoreGive(liveStreamLock);
    liveDrawHiRes = false;
    return liveDrawLen > 0;
  }
  uint32_t t0 = millis();
  if (xSemaphoreTake(liveCamLock, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, CAPTURE_FRAME_SIZE);
  int64_t since = esp_timer_get_time();
  bool ok = false;
  for (int tries = 0; tries < 8 && millis() - t0 < 1500; tries++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) continue;
    if (isFreshJpeg(fb, since, true)) {
      memcpy(liveDrawBuf, fb->buf, fb->len);
      liveDrawLen = fb->len;
      ok = true;
    }
    esp_camera_fb_return(fb);
    if (ok) break;
  }
  s->set_framesize(s, liveFrameSize);
  xSemaphoreGive(liveCamLock);
  liveStats.camMs = millis() - t0;
  if (ok) {
    liveDrawHiRes = true;
    livePublishFrame(true);          // web cung thay anh vua chup
    liveDrawnSeq = liveStreamSeq;    // khong ve lai anh VGA nhu khung live
  }
  return ok;
}

#else
// ---------- Khoi tao ----------

// Goi TRUOC lcd.init(): sau khi nap code/reset mem, cam bien van con dien va
// co the dang bat chan xuat -> phai tat no truoc khi ESP32 lai bus cho TFT.
// Tra ve true neu camera chay duoc.
static bool liveCameraBegin() {
  if (!liveAllocBuffers()) return false;
  busPinsToCamera();  // ESP32 nha bus truoc khi cam bien bat chan xuat
  camera_config_t c = liveCameraConfig();
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[CAM] esp_camera_init loi 0x%x\n", err);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  liveTuneSensor(s);
  s->set_framesize(s, liveFrameSize);
  Serial.printf("[CAM] Sensor PID=0x%04x\n", s->id.PID);

  int r1 = s->get_reg(s, OV_PAD_OEN1, 0xFF);
  int r2 = s->get_reg(s, OV_PAD_OEN2, 0xFF);
  Serial.printf("[CAM] PAD OEN 0x3017=0x%02x 0x3018=0x%02x\n", r1, r2);
  if (r1 > 0 || r2 > 0) {
    livePadOn1 = r1 > 0 ? r1 : 0xFF;
    livePadOn2 = r2 > 0 ? r2 : 0xFF;
    sensorPads(false);
    int c1 = s->get_reg(s, OV_PAD_OEN1, 0xFF), c2 = s->get_reg(s, OV_PAD_OEN2, 0xFF);
    livePadGating = (c1 == 0 && c2 == 0);
  }
  if (!livePadGating) {
    // Khong tat duoc chan cam bien -> khong de cam bien chay trong luc ve TFT.
    Serial.println("[CAM] Khong dieu khien duoc chan xuat cam bien: dung che do init/deinit moi khung (cham).");
    sensorSoftResetToIdle();
    esp_camera_deinit();
  }
  holdSyncLinesLow();
  // Bus de trong; lcd.init() ngay sau day se nhan 5 chan cho TFT.
  liveReady = true;
  Serial.printf("[CAM] San sang. Che do: %s\n", livePadGating ? "tat/bat chan SCCB (nhanh)" : "init/deinit (cham)");
  return true;
}

// ---------- Mot chu ky: chup 1 khung ----------
// hiRes=false: khung live QVGA. hiRes=true: anh chup VGA 640x480 de nhan dien.
// Tra ve true neu liveDrawBuf co khung moi. Sau ham nay bus da thuoc ve TFT.

static bool liveGrabFrame(bool hiRes = false) {
  if (!liveReady) return false;
  uint32_t t0 = millis();
  lcd.waitDisplay();
  busPinsToCamera();

  camera_fb_t* fb = nullptr;
  bool ok = false;
  if (livePadGating) {
    sensor_t* s = esp_camera_sensor_get();
    sensorPads(true);
    if (hiRes) {
      s->set_framesize(s, CAPTURE_FRAME_SIZE);
      sensorPads(true);  // doi kich thuoc co the ghi lai thanh ghi pad
    }
    int64_t phaseStart = esp_timer_get_time();
    // Bo khung cu (bat dau truoc pha nay hoac bi cat ngang luc tat chan).
    uint32_t budget = hiRes ? 1500 : 400;
    for (int tries = 0; tries < 8 && millis() - t0 < budget; tries++) {
      fb = esp_camera_fb_get();
      if (!fb) continue;
      int64_t ts = int64_t(fb->timestamp.tv_sec) * 1000000LL + fb->timestamp.tv_usec;
      bool sizeOk = !hiRes || fb->width > 320;
      if (ts >= phaseStart && sizeOk && fb->len > 4 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8)
        break;
      esp_camera_fb_return(fb); fb = nullptr; liveStats.dropped++;
    }
    if (hiRes) {
      if (fb && fb->len <= LIVE_MAX_JPEG) {  // chep truoc khi doi lai kich thuoc
        memcpy(liveDrawBuf, fb->buf, fb->len); liveDrawLen = fb->len; ok = true;
      }
      if (fb) { esp_camera_fb_return(fb); fb = nullptr; }
      s->set_framesize(s, liveFrameSize);
    }
    sensorPads(false);
  } else {
    camera_config_t c = liveCameraConfig();
    c.fb_count = 1;
    if (esp_camera_init(&c) == ESP_OK) {
      sensor_t* s = esp_camera_sensor_get();
      liveTuneSensor(s);
      if (!hiRes) s->set_framesize(s, liveFrameSize);
      esp_camera_fb_return(esp_camera_fb_get());  // khung dau thuong toi/loi
      fb = esp_camera_fb_get();
    }
  }

  if (!ok && fb && fb->len <= LIVE_MAX_JPEG) {
    memcpy(liveDrawBuf, fb->buf, fb->len);
    liveDrawLen = fb->len;
    ok = true;
  }
  if (ok) liveDrawHiRes = hiRes;
  if (fb) esp_camera_fb_return(fb);
  if (!livePadGating) { sensorSoftResetToIdle(); esp_camera_deinit(); }

  holdSyncLinesLow();
  busPinsToTft();
  uint32_t t1 = millis();
  tftRestore();
  liveStats.camMs = t1 - t0;

  livePublishFrame(ok);
  return ok;
}
#endif  // SHARED_CAM_TFT_BUS

// Ve khung moi nhat vao vung (x, y, w, h). Bus phai dang thuoc ve TFT.
// Doc kich thuoc anh tu marker SOF cua JPEG.
static bool jpegSize(const uint8_t* d, size_t n, int& w, int& h) {
  for (size_t i = 2; i + 9 < n;) {
    if (d[i] != 0xFF) return false;
    uint8_t m = d[i + 1];
    size_t len = (d[i + 2] << 8) | d[i + 3];
    if (m >= 0xC0 && m <= 0xC3) { h = (d[i + 5] << 8) | d[i + 6]; w = (d[i + 7] << 8) | d[i + 8]; return true; }
    i += 2 + len;
  }
  return false;
}

static void liveDrawFrame(int x, int y, int w, int h) {
  if (!liveDrawLen) return;
  uint32_t t0 = millis();
  float scale = liveDrawHiRes ? 0.5f : 1.0f;  // anh VGA thu nho 1/2 cho vua khung 320x240
  // Anh live nho hon khung (vd 240x176): ve giua khung, to den phan vien khi doi kich thuoc.
  static int lastW = 0, lastH = 0;
  int iw = w, ih = h;
  if (jpegSize(liveDrawBuf, liveDrawLen, iw, ih)) { iw = int(iw * scale); ih = int(ih * scale); }
  if (iw > w) iw = w;
  if (ih > h) ih = h;
  if (iw != lastW || ih != lastH) { lcd.fillRect(x, y, w, h, 0); lastW = iw; lastH = ih; }
  x += (w - iw) / 2; y += (h - ih) / 2;
  lcd.drawJpg(liveDrawBuf, liveDrawLen, x, y, iw, ih, 0, 0, scale);
  lcd.waitDisplay();
  liveStats.tftMs = millis() - t0;
}

static const char* liveRestoreName() {
  switch (liveRestoreMode) {
    case RESTORE_FAST: return "NHANH";
    case RESTORE_SAFE: return "AN TOAN";
    default: return "INIT LAI";
  }
}
