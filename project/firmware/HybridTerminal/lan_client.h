#pragma once
// HTTP LAN toi Pi (khong TLS, khong Supabase): gui anh nhan dien, tai man ket qua
// va am thanh. Cung API voi CloudTransport::lan* trong cloud_transport.h.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include "config.h"
#include "wifi_setup.h"  // lanServerHost: may chu nhap o trang cau hinh

class LanSink : public Stream {
 public:
  uint8_t* data;
  size_t capacity, length = 0;
  bool overflow = false;
  LanSink(uint8_t* buffer, size_t size) : data(buffer), capacity(size) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* buffer, size_t count) override {
    if (!data || count > capacity - length) { overflow = true; return 0; }
    memcpy(data + length, buffer, count); length += count; return count;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// =====================================================================
// THEO DOI MAY CHU (task rieng, khong chan giao dien)
// Thu tu tim: (1) IP/ten nhap o trang cau hinh - neu co thi CHI dung no,
// (2) LAN_HOST_IP_VALUE trong secrets.h, (3) ai-camera.local qua mDNS, (3b) do tim UDP
// broadcast (may tinh Windows khong co mDNS),
// (4) gateway - dung khi ESP32 vao diem phat cua chinh may chay server
//     (Windows Mobile Hotspot: 192.168.137.1).
// May chu "co" = GET http://<ip>:8000/health tra 200.
// =====================================================================
enum ServerState : uint8_t { SV_UNKNOWN, SV_OK, SV_MISSING };
static volatile ServerState serverState = SV_UNKNOWN;
static volatile uint32_t serverIpRaw = 0;      // IP may chu dang dung (khi SV_OK)
static volatile bool serverChanged = false;    // UI doc va xoa
static volatile bool serverCheckNow = false;   // yeu cau kiem tra ngay
static volatile uint32_t serverChecks = 0;
static char serverTried[120] = "";              // vua thu nhung dia chi nao (hien len TFT)

static void serverSet(ServerState s) {
  if (serverState != s) { serverState = s; serverChanged = true; }
}

static bool lanHealth(IPAddress ip) {
  if (!ip) return false;
  WiFiClient c;
  HTTPClient h;
  h.setConnectTimeout(1500);
  h.setTimeout(2500);
  if (!h.begin(c, "http://" + ip.toString() + ":" + String(LAN_PORT) + "/health")) return false;
  int code = h.GET();
  h.end();
  return code == 200;
}

static IPAddress lanResolveName(const char* name) {
  IPAddress ip;
  if (ip.fromString(name)) return ip;
  String n = name;
  if (n.endsWith(".local")) return MDNS.queryHost(n.substring(0, n.length() - 6), 1500);
  if (!WiFi.hostByName(name, ip)) ip = IPAddress();
  return ip;
}

// Hoi ca mang "AICAM?" qua UDP broadcast; may chu (app/lan_discovery.py) tra "AICAM <cong>".
static IPAddress lanDiscover() {
  WiFiUDP udp;
  if (!udp.begin(LAN_DISCOVERY_PORT + 1)) return IPAddress();
  IPAddress found;
  for (int attempt = 0; attempt < 2 && !found; attempt++) {
    udp.beginPacket(WiFi.broadcastIP(), LAN_DISCOVERY_PORT);
    udp.print("AICAM?");
    udp.endPacket();
    for (uint32_t t0 = millis(); millis() - t0 < 700 && !found;) {
      if (udp.parsePacket() > 0) {
        char b[32];
        int r = udp.read(b, sizeof b - 1);
        b[r > 0 ? r : 0] = 0;
        if (!strncmp(b, "AICAM ", 6)) found = udp.remoteIP();
      } else {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
  }
  udp.stop();
  return found;
}

static void serverMonitorTask(void*) {
  uint8_t fails = 0;
  for (;;) {
    serverCheckNow = false;
    if (WiFi.status() != WL_CONNECTED) {
      serverSet(SV_UNKNOWN);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    IPAddress cand[5];
    const char* tag[5] = {"", "", "", "", ""};
    int n = 0;
    if (lanServerHost[0]) { cand[n] = lanResolveName(lanServerHost); tag[n++] = "đã nhập"; }
    else {
      // May chu vua dung duoc thu truoc: khong phai do lai moi lan kiem tra.
      if (serverIpRaw) { cand[n] = IPAddress(serverIpRaw); tag[n++] = "lần trước"; }
#ifdef LAN_HOST_IP_VALUE
      cand[n].fromString(LAN_HOST_IP_VALUE); tag[n++] = "secrets.h";
#endif
      cand[n] = MDNS.queryHost(LAN_HOSTNAME, 1500); tag[n++] = "ai-camera.local";
      cand[n] = lanDiscover(); tag[n++] = "dò tìm";
      cand[n] = WiFi.gatewayIP(); tag[n++] = "gateway";
    }
    IPAddress found;
    String tried;
    for (int i = 0; i < n && !found; i++) {
      if (!cand[i]) continue;
      if (tried.length()) tried += ", ";
      tried += cand[i].toString() + " (" + tag[i] + ")";
      if (lanHealth(cand[i])) found = cand[i];
    }
    if (!tried.length()) tried = lanServerHost[0] ? String(lanServerHost) + " (không phân giải được)" : String("ai-camera.local");
    strlcpy(serverTried, tried.c_str(), sizeof serverTried);
    serverChecks = serverChecks + 1;
    if (found) {
      if ((uint32_t)found != serverIpRaw) Serial.printf("[LAN] May chu %s OK\n", found.toString().c_str());
      serverIpRaw = (uint32_t)found;
      fails = 0;
      serverSet(SV_OK);
    } else {
      // Mat 1 lan co the do mang chap chon: bao mat khi 2 lan lien tiep (hoac chua tung thay).
      if (++fails >= 2 || serverState != SV_OK) {
        if (serverState != SV_MISSING) Serial.printf("[LAN] Khong thay may chu (da thu: %s)\n", serverTried);
        serverSet(SV_MISSING);
      }
    }
    uint32_t gen = lanServerGen, waitMs = serverState == SV_OK ? 8000 : 3000;
    for (uint32_t t = 0; t < waitMs && !serverCheckNow && gen == lanServerGen; t += 100)
      vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static void serverMonitorBegin() {
  static bool started = false;
  if (started) return;
  started = true;
  xTaskCreatePinnedToCore(serverMonitorTask, "srvmon", 6144, nullptr, 1, nullptr, 0);
}

class LanClient {
  WiFiClient socket;

  String url(const String& path) {
    IPAddress ip = resolveHost();
    if (!ip) return "";
    return "http://" + ip.toString() + ":" + String(LAN_PORT) + path;
  }

  bool download(const String& path, uint8_t* data, size_t capacity, size_t& length) {
    length = 0;
    String target = url(path);
    if (!target.length()) return false;
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(8000);
    if (!http.begin(socket, target)) return false;
    http.addHeader("X-LAN-Key", LAN_API_KEY_VALUE);
    int code = http.GET();
    if (code == 200) {
      LanSink sink(data, capacity);
      int read = http.writeToStream(&sink);
      length = sink.length;
      if (read < 0 || sink.overflow) code = -1;
    }
    http.end();
    return code == 200;
  }

 public:
  int lastStatus = 0;

  // Doi mang WiFi / doi may chu: kiem tra lai ngay trong task theo doi.
  void forgetHost() { serverCheckNow = true; }

  // Khong tim may chu o day (mDNS chan ca vong loop) - dung ket qua task theo doi.
  IPAddress resolveHost() {
    return serverState == SV_OK ? IPAddress(serverIpRaw) : IPAddress();
  }

  bool recognize(const String& device, const String& id, const uint8_t* jpeg, size_t length,
                 bool automatic, JsonDocument& result) {
    lastStatus = 0;
    String target = url("/api/lan/device/" + device + "/recognize");
    if (!target.length()) { lastStatus = -10; return false; }  // khong tim thay Pi
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(15000);
    if (!http.begin(socket, target)) return false;
    http.addHeader("X-LAN-Key", LAN_API_KEY_VALUE);
    http.addHeader("X-Request-ID", id);
    http.addHeader("X-Capture-Mode", automatic ? "auto" : "manual");
    http.addHeader("X-Count-All", "false");
    http.addHeader("X-Retry-Index", "0");
    http.addHeader("Content-Type", "image/jpeg");
    lastStatus = http.POST(const_cast<uint8_t*>(jpeg), length);
    String text = lastStatus >= 200 && lastStatus < 300 ? http.getString() : "";
    http.end();
    return lastStatus >= 200 && lastStatus < 300 && !deserializeJson(result, text);
  }

  bool screen(const String& device, const String& id, uint8_t* data, size_t cap, size_t& len) {
    return download("/api/lan/device/" + device + "/jobs/" + id + "/screen.jpg", data, cap, len);
  }

  bool audio(const String& device, const String& id, uint8_t* data, size_t cap, size_t& len) {
    return download("/api/lan/device/" + device + "/jobs/" + id + "/audio.wav", data, cap, len);
  }

  bool ack(const String& device, const String& id, bool displayed, bool played) {
    String target = url("/api/lan/device/" + device + "/jobs/" + id + "/ack");
    if (!target.length()) return false;
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(5000);
    if (!http.begin(socket, target)) return false;
    http.addHeader("X-LAN-Key", LAN_API_KEY_VALUE);
    http.addHeader("Content-Type", "application/json");
    String body = String("{\"request_id\":\"") + id + "\",\"displayed\":" +
                  (displayed ? "true" : "false") + ",\"played\":" + (played ? "true" : "false") + "}";
    int code = http.POST(body);
    http.end();
    return code >= 200 && code < 300;
  }
};
