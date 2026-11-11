#pragma once
// =====================================================================
// DONG BO VOI DASHBOARD QUA LAN
// Task rieng, ~0,5 s/lan: POST trang thai thiet bi toi
//   http://<may chu>:8000/api/lan/device/<id>/exchange
// va nhan lenh tu web (auto, manual, start, stop, capture, home, replay, greeting).
// Lenh duoc dua vao hang cho; loop() ap dung nhu bam nut tren bo (khong ve TFT tu task nay).
// =====================================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include "config.h"
#include "lan_client.h"

// Trang thai loop() cap nhat moi vong, task doc de gui len web.
struct DevSyncState {
  volatile bool autoMode = true;
  volatile bool running = false;   // dang live / dang xu ly
  volatile bool hasFrame = false;
  volatile bool playing = false;   // loa dang doc
  volatile uint8_t view = 0;       // 0 home, 1 live, 2 busy, 3 result, 4 error, 5 wifi
  volatile float hold = 0;         // giay da giu (AUTO)
  volatile int volume = 0;
  char resultId[40] = "";          // ket qua gan nhat (id job tren may chu)
  char controlId[104] = "";        // lenh web da ap dung
};
static DevSyncState devState;

// Lenh web cho loop() ap dung
static char devCmdAction[16] = "";
static char devCmdId[104] = "";
static volatile bool devCmdPending = false;
static volatile bool devSyncOnline = false;
static volatile bool devSyncNow = false;  // loop vua ap dung lenh: bao web ngay

static const char* devViewName(uint8_t v) {
  static const char* names[] = {"home", "live", "busy", "result", "error", "wifi"};
  return v < 6 ? names[v] : "home";
}

static void devSyncTask(void*) {
  bool seeded = false;  // lan dau: nhan id lenh cu ma khong ap dung (tranh lap lai lenh truoc khi reset)
  for (;;) {
    // Cho toi luot gui (0,4 s khi dang live, 0,7 s khi ranh) - hoac gui ngay sau khi ap dung lenh.
    for (uint32_t t = 0, wait = devState.running ? 400 : 700; t < wait && !devSyncNow; t += 50)
      vTaskDelay(pdMS_TO_TICKS(50));
    devSyncNow = false;
    if (serverState != SV_OK || WiFi.status() != WL_CONNECTED) { devSyncOnline = false; continue; }
    if (devCmdPending) continue;  // loop chua ap dung lenh truoc

    JsonDocument st;
    st["mode"] = devState.autoMode ? "auto" : "manual";
    st["running"] = (bool)devState.running;
    st["view"] = devViewName(devState.view);
    st["hold"] = (float)devState.hold;
    st["has_frame"] = (bool)devState.hasFrame;
    st["result_id"] = devState.resultId;
    st["control_id"] = devState.controlId;
    st["audio_status"] = devState.playing ? "playing" : "idle";
    st["volume"] = (int)devState.volume;
    String body;
    serializeJson(st, body);

    WiFiClient c;
    HTTPClient h;
    h.setConnectTimeout(1200);
    h.setTimeout(2500);
    String url = "http://" + IPAddress(serverIpRaw).toString() + ":" + String(LAN_PORT) +
                 "/api/lan/device/" + String(DEVICE_ID) + "/exchange";
    if (!h.begin(c, url)) continue;
    h.addHeader("Content-Type", "application/json");
    h.addHeader("X-LAN-Key", LAN_API_KEY_VALUE);
    int code = h.POST(body);
    String reply = code == 200 ? h.getString() : String();
    h.end();
    devSyncOnline = code == 200;
    if (code != 200) {
      static uint32_t lastLog = 0;
      if (millis() - lastLog > 30000) { lastLog = millis(); Serial.printf("[SYNC] exchange loi %d\n", code); }
      continue;
    }

    JsonDocument snap;
    if (deserializeJson(snap, reply)) continue;
    const char* id = snap["control"]["id"] | "";
    const char* action = snap["control"]["action"] | "";
    if (!id[0] || !action[0] || !strcmp(id, devState.controlId)) { seeded = true; continue; }
    if (!seeded) {  // lenh con treo tu truoc khi khoi dong: bo qua
      strlcpy(devState.controlId, id, sizeof devState.controlId);
      seeded = true;
      continue;
    }
    strlcpy(devCmdAction, action, sizeof devCmdAction);
    strlcpy(devCmdId, id, sizeof devCmdId);
    devCmdPending = true;
    Serial.printf("[SYNC] Lenh tu web: %s\n", action);
  }
}

static void devSyncBegin() {
  static bool started = false;
  if (started) return;
  started = true;
  xTaskCreatePinnedToCore(devSyncTask, "devsync", 8192, nullptr, 1, nullptr, 0);
}
