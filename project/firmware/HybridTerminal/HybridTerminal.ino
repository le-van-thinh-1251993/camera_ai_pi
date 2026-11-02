#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "protocol_types.h"
#include "cloud_transport.h"
#include "hardware.h"

static CloudTransport cloud;
static Preferences preferences;
static Snapshot shared;
static SemaphoreHandle_t stateMutex, jpegMutex, screenMutex;
static QueueHandle_t commands;
static uint8_t *cachedJpeg = nullptr, *screenJpeg = nullptr;
static size_t cachedSize = 0, screenSize = 0;
static uint32_t cachedAt = 0;
static CapturedFrame frames[AUTO_FRAMES];
static std::atomic<bool> cancelAudio{false}, menuOpen{true};
static std::atomic<bool> screenPresented{false};
static bool newScreen = false, hardwareOk = false;
// These strings and protocol state are owned exclusively by the network task.
static String pendingId, lastConsumed, audioPath, lastControl;
static unsigned frameCount = 0, targetFrames = 0, attempt = 0;
static bool collecting = false, submitted = false, collectingAuto = false;
static bool localSubmitted = false;
static bool systemOn = true, greetingShown = false;
static JsonDocument localJob;
static bool manualPersisted = false;
static uint32_t pollAt = 0, syncAt = 0;
static uint32_t captureStarted = 0;
static uint32_t requestStarted = 0;
static JsonDocument ackQueue;

static uint8_t *allocateBytes(size_t bytes) {
  auto memory = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return memory ? memory : (uint8_t *)malloc(bytes);
}
static Snapshot snapshot() {
  xSemaphoreTake(stateMutex, portMAX_DELAY); Snapshot value = shared;
  xSemaphoreGive(stateMutex); return value;
}
static void setState(const Snapshot &value) {
  xSemaphoreTake(stateMutex, portMAX_DELAY); shared = value; xSemaphoreGive(stateMutex);
}
static String uuid() {
  uint8_t bytes[16]; esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = (bytes[6] & 15) | 0x40; bytes[8] = (bytes[8] & 63) | 0x80;
  char output[37];
  snprintf(output, sizeof(output), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
  return String(output);
}
static void storeAcks() {
  String text; serializeJson(ackQueue, text); preferences.putString("acks", text);
}
static void queueAck(const String &id, bool displayed, bool played) {
  JsonObject row;
  for(JsonObject item:ackQueue.as<JsonArray>())if(item["id"].as<String>()==id){row=item;break;}
  if(row.isNull())row=ackQueue.as<JsonArray>().add<JsonObject>();
  displayed=displayed||(row["displayed"]|false);played=played||(row["played"]|false);
  row["id"] = id; row["displayed"] = displayed; row["played"] = played; storeAcks();
}
static void flushAck() {
  static uint32_t next = 0;
  if (!ackQueue.size() || int32_t(millis() - next) < 0 || !cloud.login()) return;
  JsonDocument body, output; JsonObject row = ackQueue[0];
  body["p_id"] = row["id"]; body["p_displayed"] = row["displayed"]; body["p_played"] = row["played"];
  String text; serializeJson(body, text);
  int code=cloud.json("POST", "/rest/v1/rpc/ai_ack", text, output);
  bool consumed=code==200&&output.as<bool>();
  if(code==200&&!consumed){
    JsonDocument rows;
    if(cloud.json("GET","/rest/v1/ai_requests?id=eq."+row["id"].as<String>()+"&select=id&limit=1","",rows)==200&&rows.size()==0)consumed=true;
  }
  if (consumed) {
    ackQueue.as<JsonArray>().remove(0); storeAcks(); next = 0;
  } else next = millis() + 15000;
}
static void resetRequest() {
  collecting = false; submitted = false; localSubmitted = false; pendingId = ""; frameCount = 0;
  if(manualPersisted){preferences.remove("pending");manualPersisted=false;}
}
static void beginCapture(bool automatic, bool newRound) {
  if (ackQueue.size() >= 32) { Serial.println("ACK queue full; reconnect Cloud first."); return; }
  for(auto &frame:frames)if(!frame.data){Serial.println("Not enough RAM. Verify PSRAM settings.");return;}
  if (newRound) attempt = 0;
  pendingId = uuid(); frameCount = 0; targetFrames = 1;
  collecting = true; collectingAuto = automatic; submitted = false;
  captureStarted=millis();
  requestStarted=0;
  Snapshot value = snapshot(); value.resultId = ""; value.hold = 0;
  value.view = "preview"; value.running = automatic; setState(value);
}
static void captureStep() {
  if(collecting&&millis()-captureStarted>6000){
    collecting=false;pendingId="";Snapshot value=snapshot();value.running=false;value.view="paused";value.text="Camera chua co anh. Kiem tra camera va GPIO.";setState(value);return;
  }
  if (!collecting || !frames[frameCount].data) return;
  if (collectingAuto && millis()-captureStarted < 2000) {
    Snapshot value=snapshot();value.hold=(millis()-captureStarted)/1000.f;setState(value);return;
  }
  xSemaphoreTake(jpegMutex, portMAX_DELAY);
  bool ready = cachedSize && (millis() - cachedAt < 500) &&
               (frameCount == 0 || uint32_t(cachedAt - frames[0].captured) >= frameCount * 1000);
  if (ready) {
    frames[frameCount].size = cachedSize; frames[frameCount].captured = cachedAt;
    memcpy(frames[frameCount].data, cachedJpeg, cachedSize); frameCount++;
  }
  xSemaphoreGive(jpegMutex);
  Snapshot value = snapshot();
  if (frameCount) value.hold = min(2.f, uint32_t(millis() - frames[0].captured) / 1000.f);
  if (frameCount == targetFrames) { collecting = false; value.view = "processing"; value.hold = collectingAuto ? 2.f : 0.f; }
  setState(value);
}
static String inputDocument() {
  Snapshot value = snapshot(); JsonDocument input;
  input["pipeline_version"] = 6; input["source"] = "esp32";
  input["mode"] = "manual";
  input["capture_mode"] = collectingAuto ? "auto" : "manual";
  input["count_all"] = value.countAll; input["retry_index"] = attempt;
  for (unsigned i = 0; i < targetFrames; i++) {
    JsonObject row = input["frames"].add<JsonObject>();
    row["path"] = String(SB_UID) + "/" + pendingId + (i ? "/" + String(i) + ".jpg" : ".jpg");
    row["ms"] = uint32_t(frames[i].captured - frames[0].captured);
  }
  String text; serializeJson(input, text); return text;
}
static bool submitRequest() {
  if (!cloud.login()) return false;
  JsonDocument input; deserializeJson(input, inputDocument());
  for (unsigned i = 0; i < targetFrames; i++) {
    if (!cloud.image(input["frames"][i]["path"].as<String>(), frames[i].data, frames[i].size)) return false;
  }
  JsonDocument args, output;
  args["p_id"] = pendingId; args["p_device"] = DEVICE_ID;
  args["p_image"] = String(SB_UID) + "/" + pendingId + ".jpg"; args["p_input"].set(input);
  String text; serializeJson(args, text);
  if (cloud.json("POST", "/rest/v1/rpc/ai_submit", text, output) != 200) return false;
  // Auto probes stay in RAM. Manual can resume an accepted Cloud UUID after reboot.
  if (!collectingAuto) {preferences.putString("pending", pendingId);manualPersisted=true;}
  return true;
}
static bool submitLocalRequest() {
  Snapshot value = snapshot();
  localJob.clear();
  if (!cloud.lanRecognize(DEVICE_ID, pendingId, frames[0].data, frames[0].size,
                           collectingAuto, value.countAll, attempt, localJob)) return false;
  if (localJob["result"]["id"].as<String>() != pendingId) return false;
  localSubmitted = true;
  return true;
}
static bool playAudio(const String &path) {
  auto wav = allocateBytes(MAX_WAV_BYTES); size_t length = 0;
  if (!wav) return false;
  cancelAudio = false;
  bool downloaded = localSubmitted
    ? cloud.lanAudio(DEVICE_ID, pendingId, wav, MAX_WAV_BYTES, length)
    : cloud.download(path, wav, MAX_WAV_BYTES, length);
  bool played = downloaded && playWav(wav, length, cancelAudio);
  free(wav); return played;
}
static void finalResult(JsonDocument &job) {
  menuOpen=false;screenPresented=false;
  String id = job["id"] | ""; bool displayed = false, played = false;
  Snapshot value = snapshot(); value.running = false; value.view = "result"; value.resultId = id;
  value.text = job["result"]["text"] | "Chua nhan dien duoc cu chi"; setState(value);
  bool screenLoaded = false;
  String screenPath = job["screen_path"] | "";
  if (screenJpeg && ((localSubmitted && screenPath == "local") ||
                     (!localSubmitted && screenPath.startsWith(String(SB_UID) + "/")))) {
    size_t downloaded = 0;
    screenLoaded = localSubmitted
      ? cloud.lanScreen(DEVICE_ID, id, screenJpeg, MAX_SCREEN_BYTES, downloaded)
      : cloud.download(screenPath, screenJpeg, MAX_SCREEN_BYTES, downloaded);
    if (screenLoaded) {
      xSemaphoreTake(screenMutex, portMAX_DELAY);
      screenSize = downloaded; newScreen = true;
      xSemaphoreGive(screenMutex);
    }
  }
  if (!screenLoaded) drawResultView(job["result"]["gesture"] | "Unknown", value.text);
  displayed = lcdReady;
  audioPath = job["audio_path"] | "";
  if (lastConsumed != id) {
    lastConsumed = id; preferences.putString("consumed", id);
    preferences.putBool("played", false); played = playAudio(audioPath);
    preferences.putBool("played", played);
  } else played = preferences.getBool("played", false);
  if (localSubmitted) cloud.lanAck(DEVICE_ID, id, displayed, played);
  else queueAck(id, displayed, played);
  Serial.println(value.text); resetRequest();
}
static void handleAction(const Action &action) {
  String name = action.name; Snapshot value = snapshot(); cancelAudio = true;
  if (name == "power") {
    systemOn = !systemOn;
    if (!systemOn) { value.running = false; value.view = "standby"; setState(value); resetRequest(); drawStandbyView(); }
    else { value.view = "greeting"; setState(value); drawGreetingScreen(); }
    return;
  }
  if (!systemOn) return;
  if (name == "volume_up") { currentVolumePercent = min(100, currentVolumePercent + 10); return; }
  if (name == "volume_down") { currentVolumePercent = max(0, currentVolumePercent - 10); return; }
  if (name == "next") {
    resetRequest(); menuOpen = false; value.mode = "auto"; value.view = "preview"; value.running = true; setState(value);
    beginCapture(true, true); return;
  }
  if (action.hasCount) value.countAll = action.countAll;
  if (name == "toggle_count") { value.countAll = !value.countAll; setState(value); return; }
  if (name == "replay") { playAudio(audioPath); return; }
  if (name == "greeting") { if (cloud.login()) playAudio(String(SB_UID) + "/greeting/voice.wav"); return; }
  resetRequest(); value.hold = 0; value.resultId = "";
  if (name == "home") { value.view = "home"; value.running = false; }
  else if (name == "stop") { value.view = "paused"; value.running = false; }
  else if (name == "auto") { menuOpen = false; value.mode = name; value.view = "preview"; value.running = true; }
  else if (name == "manual") { menuOpen = false; value.mode = name; value.view = "manual"; value.running = false; }
  else if (name == "start") { menuOpen = false; value.view = "preview"; value.running = true; }
  setState(value);
  if (name == "manual") drawManualView();
  if (name == "capture" && value.mode == "manual") beginCapture(false, true);
  else if (value.mode == "auto" && value.running) beginCapture(true, true);
}
static void exchangeState() {
  if (int32_t(millis() - syncAt) < 0) return;
  Snapshot value = snapshot(); JsonDocument args, output;
  args["p_device"] = DEVICE_ID;
  JsonObject state = args["p_state"].to<JsonObject>();
  state["mode"] = value.mode; state["running"] = value.running; state["view"] = value.view;
  state["hold"] = value.hold; state["result_id"] = value.resultId; state["control_id"] = lastControl;
  state["count_all"] = value.countAll;
  xSemaphoreTake(jpegMutex, portMAX_DELAY); state["has_frame"] = cachedSize > 0; xSemaphoreGive(jpegMutex);
  state["audio_status"] = value.view == "result" ? (ENABLE_I2S_AUDIO ? (preferences.getBool("played", false) ? "played" : "failed") : "disabled") : "idle";
  String body; serializeJson(args, body);
  bool exchanged = cloud.login() &&
                   cloud.json("POST", "/rest/v1/rpc/ai_device_exchange", body, output) == 200;
  if (!exchanged) exchanged = cloud.lanJson(
      "/api/lan/device/" + String(DEVICE_ID) + "/exchange", body, output);
  if (exchanged) {
    String id = output["control"]["id"] | "";
    if (id.length() && id != lastControl) {
      Action action = {}; String name = output["control"]["action"] | "";
      strlcpy(action.name, name.c_str(), sizeof(action.name));
      action.hasCount = true; action.countAll = output["control"]["count_all"] | false;
      handleAction(action); lastControl = id; preferences.putString("control", id);
      menuOpen = name == "home";
    }
  }
  syncAt = millis() + (value.running ? 5000 : 15000);
}
static void networkTask(void *) {
  preferences.begin("ai-hybrid", false);
  lastConsumed = preferences.getString("consumed", ""); lastControl = preferences.getString("control", "");
  if (deserializeJson(ackQueue, preferences.getString("acks", "[]"))) ackQueue.to<JsonArray>();
  pendingId = preferences.getString("pending", ""); submitted = pendingId.length();
  manualPersisted=submitted;
  collectingAuto = false; unsigned failures = 0;
  for (;;) {
    Action action;
    if (xQueueReceive(commands, &action, 0) == pdTRUE) { handleAction(action); syncAt = 0; failures = 0; }
    if (!systemOn) { delay(100); continue; }
    if (collecting) { captureStep(); delay(15); continue; } // Do not let TLS latency stretch the capture burst.
    if (WiFi.status() != WL_CONNECTED) { delay(200); continue; }
    if (!greetingShown) {
      greetingShown = true;
      drawGreetingScreen();
#if ENABLE_I2S_AUDIO
      if (cloud.login()) playAudio(String(SB_UID) + "/greeting/voice.wav");
#endif
    }
    exchangeState(); flushAck();
    if (int32_t(millis() - pollAt) < 0) { delay(30); continue; }
    if (!pendingId.length()) { delay(100); continue; }
    if (requestStarted && millis() - requestStarted > RESULT_TIMEOUT_MS) {
      Snapshot value = snapshot(); value.running = false; value.view = "paused";
      value.text = "Het thoi gian cho ket qua. Vui long thu lai.";
      setState(value); resetRequest(); requestStarted = 0; continue;
    }
    if (!submitted) {
      localSubmitted = false;
      if (!submitRequest() && !submitLocalRequest()) {
        if (++failures >= 4) { Snapshot value = snapshot(); value.running = false; value.view = "paused"; value.text = "Khong ket noi duoc Supabase"; setState(value); resetRequest(); }
        pollAt = millis() + 3000; continue;
      }
      submitted = true; requestStarted = millis(); failures = 0;
    }
    if (localSubmitted) {
      JsonDocument job; job.set(localJob);
      if (job["result"]["confirmed"] | false) finalResult(job);
      else if (collectingAuto) { resetRequest(); attempt++; beginCapture(true, false); }
      pollAt = millis() + 900;
      continue;
    }
    JsonDocument rows;
    int code = cloud.json("GET", "/rest/v1/ai_requests?id=eq." + pendingId + "&select=id,status,result,audio_path,screen_path&limit=1", "", rows);
    if (code == 200 && rows.size() == 1) {
      String status = rows[0]["status"] | "";
      if (status == "completed" || status == "acked") {
        JsonDocument job; job.set(rows[0]);
        if (job["result"]["confirmed"] | false) finalResult(job);
        else if (collectingAuto) { resetRequest(); attempt++; beginCapture(true, false); }
      } else if (status == "failed") {
        Snapshot value = snapshot(); value.running = false; value.view = "paused"; value.text = "Cloud khong xu ly duoc anh"; setState(value); resetRequest();
      }
    }
    pollAt = millis() + 900;
  }
}
struct Button {
  int pin; bool stable=false, raw=false; uint32_t changed=0;
  bool press() {
    if (pin < 0) return false;
    bool next = digitalRead(pin) == (BUTTON_ACTIVE_LOW ? LOW : HIGH);
    if (next != raw) { raw=next; changed=millis(); }
    if (next != stable && millis()-changed >= DEBOUNCE_MS) { stable=next; return stable; }
    return false;
  }
};
static Button buttons[]={{BTN_POWER},{BTN_NEXT},{BTN_VOLUME_UP},{BTN_VOLUME_DOWN},{BTN_MANUAL}};
static void enqueue(const char *name) { Action action={}; strlcpy(action.name,name,sizeof(action.name)); xQueueSend(commands,&action,0); }
static void buttonEvent(int index) {
  const char *actions[]={"power","next","volume_up","volume_down","manual"};
  if (index == 2) {
    cancelAudio = true; currentVolumePercent = min(100, currentVolumePercent + 10); return;
  }
  if (index == 3) {
    cancelAudio = true; currentVolumePercent = max(0, currentVolumePercent - 10); return;
  }
  cancelAudio = true;
  enqueue(actions[index]);
}
void setup() {
  Serial.begin(115200); delay(200);
  stateMutex=xSemaphoreCreateMutex(); jpegMutex=xSemaphoreCreateMutex(); screenMutex=xSemaphoreCreateMutex(); commands=xQueueCreate(8,sizeof(Action));
  cachedJpeg=allocateBytes(MAX_JPEG_BYTES); screenJpeg=TFT_KIND?allocateBytes(MAX_SCREEN_BYTES):nullptr;
  for(auto &frame:frames) frame.data=allocateBytes(MAX_JPEG_BYTES);
  hardwareOk=beginHardware();
  if(hardwareOk)for(auto &button:buttons)if(button.pin>=0)pinMode(button.pin,BUTTON_ACTIVE_LOW?INPUT_PULLUP:INPUT_PULLDOWN);
  drawGreetingScreen(); delay(1200);
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.persistent(true);
  if(strlen(WIFI_SSID))WiFi.begin(WIFI_SSID,WIFI_PASSWORD);else WiFi.begin();
  MDNS.begin(DEVICE_ID);
  configTime(0,0,"pool.ntp.org","time.google.com");
  Serial.println("Cloud first. Auto: one RAM frame after a 2-second countdown. p/n/+/ -/m = power/next/volume/manual.");
  xTaskCreate(networkTask,"cloud",16384,nullptr,1,nullptr);
}
void loop() {
  if(hardwareOk)for(int i=0;i<5;i++)if(buttons[i].press())buttonEvent(i);
  while(Serial.available()){char value=Serial.read();if(value=='p')buttonEvent(0);if(value=='n')buttonEvent(1);if(value=='+')buttonEvent(2);if(value=='-')buttonEvent(3);if(value=='m')buttonEvent(4);}
  if(!menuOpen){xSemaphoreTake(screenMutex,portMAX_DELAY);if(newScreen){drawScreen(screenJpeg,screenSize);if(lcdReady)screenPresented=true;newScreen=false;}xSemaphoreGive(screenMutex);}
#if ENABLE_CAMERA
  Snapshot value=snapshot(); static uint32_t last=0;
  if(cameraReady&&cachedJpeg&&value.running&&!menuOpen&&millis()-last>=PREVIEW_INTERVAL_MS){
    auto frame=esp_camera_fb_get();
    if(frame&&frame->format==PIXFORMAT_JPEG&&frame->len<=MAX_JPEG_BYTES){
      drawCamera(frame->buf,frame->len,value.hold);xSemaphoreTake(jpegMutex,portMAX_DELAY);
      memcpy(cachedJpeg,frame->buf,frame->len);cachedSize=frame->len;cachedAt=millis();xSemaphoreGive(jpegMutex);
    }
    if(frame)esp_camera_fb_return(frame);last=millis();
  }
#endif
  delay(5);
}
