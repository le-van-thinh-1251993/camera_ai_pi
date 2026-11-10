#pragma once
// HTTP LAN toi Pi (khong TLS, khong Supabase): gui anh nhan dien, tai man ket qua
// va am thanh. Cung API voi CloudTransport::lan* trong cloud_transport.h.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include "config.h"

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

class LanClient {
  WiFiClient socket;
  IPAddress host;
  uint32_t hostAt = 0;

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

  IPAddress resolveHost() {
#ifdef LAN_HOST_IP_VALUE
    IPAddress fixed;  // IP co dinh trong secrets.h
    if (fixed.fromString(LAN_HOST_IP_VALUE)) return fixed;
#endif
    if (host && millis() - hostAt < 60000) return host;
    host = MDNS.queryHost(LAN_HOSTNAME, 2000);
    hostAt = millis();
    return host;
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
