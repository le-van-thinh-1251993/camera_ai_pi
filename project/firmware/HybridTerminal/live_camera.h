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

static void liveStartHttp() {
  if (liveCtrlServer) return;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = LIVE_HTTP_PORT; cfg.ctrl_port = 32768;
  if (httpd_start(&liveCtrlServer, &cfg) == ESP_OK) {
    httpd_uri_t cap = {"/capture", HTTP_GET, liveCaptureHandler, nullptr};
    httpd_uri_t st = {"/status", HTTP_GET, liveStatusHandler, nullptr};
    httpd_register_uri_handler(liveCtrlServer, &cap);
    httpd_register_uri_handler(liveCtrlServer, &st);
  }
  // Stream chan luong rieng (giong CameraWebServer) de /status van tra loi.
  cfg.server_port = LIVE_HTTP_PORT + 1; cfg.ctrl_port = 32769;
  if (httpd_start(&liveStreamServer, &cfg) == ESP_OK) {
    httpd_uri_t s = {"/stream", HTTP_GET, liveStreamHandler, nullptr};
    httpd_register_uri_handler(liveStreamServer, &s);
  }
  Serial.printf("[LIVE] http://%s:%u/stream  (http://%s.local:%u/stream)\n",
                WiFi.localIP().toString().c_str(), LIVE_HTTP_PORT + 1, DEVICE_ID, LIVE_HTTP_PORT + 1);
}

static void liveWifiBegin() {
  if (!strlen(WIFI_SSID)) { Serial.println("[WIFI] Chua co WIFI_SSID trong secrets.h - bo qua stream."); return; }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // giam tre stream
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WIFI] Dang ket noi %s...\n", WIFI_SSID);
}

// Goi trong loop: khi co WiFi thi mo mDNS + HTTP mot lan.
static void liveWifiTick() {
  static bool started = false;
  if (started || WiFi.status() != WL_CONNECTED) return;
  started = true;
  MDNS.begin(DEVICE_ID);
  MDNS.addService("http", "tcp", LIVE_HTTP_PORT);
  liveStartHttp();
}

// Khung moi trong liveDrawBuf: chep sang stream va cap nhat thong ke.
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

static bool liveCameraBegin() {
  if (!liveAllocBuffers()) return false;
  camera_config_t c = liveCameraConfig();
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[CAM] esp_camera_init loi 0x%x\n", err);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, CAM_VFLIP);
  s->set_hmirror(s, CAM_HMIRROR);
  s->set_framesize(s, LIVE_FRAME_SIZE);
  Serial.printf("[CAM] Sensor PID=0x%04x - TFT bus rieng, chay dong thoi\n", s->id.PID);
  liveReady = true;
  return true;
}

// hiRes=false: khung live QVGA moi nhat. hiRes=true: chup VGA 640x480 de nhan dien.
static bool liveGrabFrame(bool hiRes = false) {
  if (!liveReady) return false;
  uint32_t t0 = millis();
  sensor_t* s = esp_camera_sensor_get();
  if (hiRes) s->set_framesize(s, CAPTURE_FRAME_SIZE);
  int64_t since = hiRes ? esp_timer_get_time() : 0;
  uint32_t budget = hiRes ? 1500 : 300;
  bool ok = false;
  for (int tries = 0; tries < 8 && millis() - t0 < budget; tries++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) continue;
    if (isFreshJpeg(fb, since, hiRes)) {
      memcpy(liveDrawBuf, fb->buf, fb->len);
      liveDrawLen = fb->len;
      ok = true;
    }
    esp_camera_fb_return(fb);
    if (ok) break;
  }
  if (hiRes) s->set_framesize(s, LIVE_FRAME_SIZE);
  if (ok) liveDrawHiRes = hiRes;
  liveStats.camMs = millis() - t0;
  livePublishFrame(ok);
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
  s->set_vflip(s, CAM_VFLIP);
  s->set_hmirror(s, CAM_HMIRROR);
  s->set_framesize(s, LIVE_FRAME_SIZE);
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
      s->set_framesize(s, LIVE_FRAME_SIZE);
    }
    sensorPads(false);
  } else {
    camera_config_t c = liveCameraConfig();
    c.fb_count = 1;
    if (esp_camera_init(&c) == ESP_OK) {
      sensor_t* s = esp_camera_sensor_get();
      s->set_vflip(s, CAM_VFLIP); s->set_hmirror(s, CAM_HMIRROR);
      if (!hiRes) s->set_framesize(s, LIVE_FRAME_SIZE);
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
static void liveDrawFrame(int x, int y, int w, int h) {
  if (!liveDrawLen) return;
  uint32_t t0 = millis();
  // Anh VGA thu nho 1/2 cho vua khung 320x240.
  lcd.drawJpg(liveDrawBuf, liveDrawLen, x, y, w, h, 0, 0, liveDrawHiRes ? 0.5f : 1.0f);
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
