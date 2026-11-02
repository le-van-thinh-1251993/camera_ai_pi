#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <time.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#ifndef CLOUD_HTTP_TIMEOUT_MS
#define CLOUD_HTTP_TIMEOUT_MS 8000
#endif

class BoundedSink : public Stream {
 public:
  uint8_t *data;
  size_t capacity, length = 0;
  bool overflow = false;
  BoundedSink(uint8_t *buffer, size_t size) : data(buffer), capacity(size) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t *buffer, size_t count) override {
    if (count > capacity - length) { overflow = true; return 0; }
    memcpy(data + length, buffer, count); length += count; return count;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

class CloudTransport {
  NetworkClientSecure socket;
  HTTPClient http;
  String token;
  uint32_t expires = 0, retryAfter = 0;
 public:
  CloudTransport() {
    socket.useBuiltinCACertBundle();
    socket.setHandshakeTimeout(8);
  }
  int request(const char *method, const String &path, const uint8_t *body,
              size_t bytes, const char *mime, uint8_t *output, size_t cap,
              size_t &length, bool authenticated = true) {
    length = 0;
    if (!path.startsWith("/") || path.startsWith("//") ||
        !String(SB_URL).startsWith("https://")) return -1;
    if (time(nullptr) < 1700000000 || int32_t(millis() - retryAfter) < 0) return -2;
    http.setReuse(true); // Keep the same TLS socket for sequential Supabase REST calls.
    http.setConnectTimeout(5000); http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
    if (!http.begin(socket, String(SB_URL) + path)) return -3;
    http.addHeader("apikey", SB_KEY);
    if (authenticated) http.addHeader("Authorization", "Bearer " + token);
    http.addHeader("Content-Type", mime);
    int code = http.sendRequest(method, const_cast<uint8_t *>(body), bytes);
    if (code > 0) {
      if (http.getSize() > int(cap)) code = -4;
      else {
        BoundedSink sink(output, cap);
        int read = http.writeToStream(&sink);
        if (read < 0 || sink.overflow) code = -5;
        else length = sink.length;
      }
    }
    http.end();
    if (code < 0) { socket.stop(); retryAfter = millis() + 10000; }
    if (code == 401) { token = ""; expires = 0; }
    return code;
  }
  int json(const char *method, const String &path, const String &body,
           JsonDocument &result, bool authenticated = true) {
    auto buffer = (uint8_t *)heap_caps_malloc(24000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) buffer = (uint8_t *)malloc(24000);
    if (!buffer) return -6;
    size_t size;
    int code = request(method, path, (const uint8_t *)body.c_str(), body.length(),
                       "application/json", buffer, 24000, size, authenticated);
    if (code >= 200 && code < 300 && size && deserializeJson(result, buffer, size)) code = -7;
    free(buffer); return code;
  }
  bool login() {
    if (int32_t(millis() - retryAfter) < 0) return false;
    if (token.length() && int32_t(expires - millis()) > 60000) return true;
    JsonDocument input, result;
    input["email"] = SB_EMAIL; input["password"] = SB_PASSWORD;
    String body; serializeJson(input, body);
    if (json("POST", "/auth/v1/token?grant_type=password", body, result, false) != 200) return false;
    token = result["access_token"] | "";
    expires = millis() + uint32_t(result["expires_in"] | 3600) * 1000;
    return token.length();
  }
  bool image(const String &path, const uint8_t *data, size_t length) {
    uint8_t response[1000]; size_t count;
    int code = request("POST", "/storage/v1/object/ai-images/" + path, data,
                       length, "image/jpeg", response, sizeof(response), count);
    if (code == 200 || code == 201) return true;
    if (code == 400 || code == 409) { // An immutable UUID upload may be retried.
      JsonDocument info;
      return json("GET", "/storage/v1/object/info/ai-images/" + path, "", info) == 200;
    }
    return false;
  }
  bool download(const String &path, uint8_t *data, size_t capacity, size_t &length) {
    if (!path.startsWith(String(SB_UID) + "/")) return false;
    return request("GET", "/storage/v1/object/authenticated/ai-results/" + path,
                   nullptr, 0, "application/octet-stream", data, capacity, length) == 200;
  }
};
