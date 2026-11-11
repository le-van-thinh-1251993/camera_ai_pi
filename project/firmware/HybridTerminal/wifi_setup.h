#pragma once
// =====================================================================
// CAU HINH WIFI (theo cach Xiaozhi ESP32: hotspot rieng + trang cau hinh)
//
//  Khoi dong  -> thu lan luot cac mang da luu (moi nhat truoc), 12 s moi mang.
//                Khong co mang nao / khong vao duoc -> bat hotspot AI-Camera-XXXX.
//  Mat WiFi   -> tu ket noi lai; qua 30 s van mat thi bat hotspot song song,
//                van tiep tuc thu cac mang da luu.
//  Hotspot    -> DNS tra moi ten mien ve 192.168.4.1 nen dien thoai tu mo trang.
//  Trang cau hinh luon co o http://<ip>/ (trong mang) va http://192.168.4.1/
//  (khi hotspot bat). Mat khau luu trong NVS (Preferences "wifi"), toi da 5 mang.
//  WIFI_SSID_VALUE trong secrets.h chi la mang mac dinh, thu sau cung.
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_http_server.h>
#include <freertos/semphr.h>
#include "config.h"
#include "live_camera.h"

enum WifiState : uint8_t { WS_CONNECTING, WS_ONLINE, WS_LOST, WS_PORTAL };
enum PortalReason : uint8_t { PR_NONE, PR_NO_WIFI, PR_LOST, PR_MANUAL };
// Man TFT ve WiFi (dung trong HybridTerminal.ino): mat mang / hotspot / vua ket noi lai
enum WifiView : uint8_t { WV_LOST, WV_PORTAL, WV_BACK, WV_NOSERVER };

static constexpr int WIFI_MAX_SAVED = 5;
static constexpr uint32_t WIFI_ATTEMPT_MS = 12000;      // thoi gian thu moi mang
static constexpr uint32_t WIFI_USER_ATTEMPT_MS = 25000; // mang nguoi dung vua luu
static constexpr uint32_t WIFI_LOST_TO_AP_MS = 30000;   // mat WiFi bao lau thi bat hotspot
static constexpr uint32_t WIFI_PORTAL_RETRY_MS = 20000; // nghi giua cac lan thu khi dang bat hotspot
static constexpr uint32_t WIFI_AP_LINGER_MS = 20000;    // giu hotspot sau khi co mang de dien thoai doc IP moi

struct SavedNet {
  char ssid[33];
  char pass[65];
};

// May chu nhan dien (Pi / may tinh chay server). Rong = tu tim (xem lan_client.h).
static char lanServerHost[40] = "";
static volatile uint32_t lanServerGen = 0;  // tang moi lan doi -> LanClient bo IP da nho
static char pendServer[40];
static volatile bool pendServerReq = false;

static SavedNet wifiNets[WIFI_MAX_SAVED + 1];  // +1: mang mac dinh trong secrets.h
static int wifiNetCount = 0;
static volatile WifiState wifiState = WS_CONNECTING;
static volatile PortalReason portalReason = PR_NONE;
static volatile bool wifiChanged = true;  // UI doc va xoa co nay
static char wifiSsid[33] = "";    // mang dang dung / dang thu
static char wifiIp[16] = "";      // IP hien tai
static char wifiOldIp[16] = "";   // IP truoc khi mat mang
static char wifiFail[33] = "";    // mang nguoi dung vua luu nhung khong vao duoc
static char apSsid[24] = "AI-Camera";
static volatile bool apOn = false;
static volatile bool wifiAttempting = false, wifiUserAttempt = false;
static uint32_t wifiStateAt = 0, wifiAttemptAt = 0, wifiIdleAt = 0, wifiLostSince = 0, apOffAt = 0;
static int wifiAttemptIdx = -1;
static DNSServer wifiDns;
static Preferences wifiPrefs;
static SemaphoreHandle_t wifiLock = nullptr;

// Lenh tu trang web (chay tren task HTTP) -> loop() xu ly.
static char pendSsid[33], pendPass[65], pendForget[33];
static volatile bool pendSave = false, pendForgetReq = false, scanReq = false;
static bool scanStarted = false;
static uint32_t scanStartAt = 0;
static String scanJson = "[]";
static volatile bool scanBusy = false;

static void wifiSetState(WifiState s) {
  wifiState = s;
  wifiStateAt = millis();
  wifiChanged = true;
}

// ---------- Danh sach mang da luu (NVS) ----------

static void wifiLoad() {
  wifiNetCount = 0;
  wifiPrefs.begin("wifi", true);
  int n = constrain(wifiPrefs.getInt("n", 0), 0, WIFI_MAX_SAVED);
  char key[4];
  for (int i = 0; i < n; i++) {
    SavedNet& w = wifiNets[wifiNetCount];
    snprintf(key, sizeof key, "s%d", i);
    if (!wifiPrefs.getString(key, w.ssid, sizeof w.ssid)) continue;
    snprintf(key, sizeof key, "p%d", i);
    if (!wifiPrefs.getString(key, w.pass, sizeof w.pass)) w.pass[0] = 0;
    if (strlen(w.ssid)) wifiNetCount++;
  }
  if (!wifiPrefs.getString("server", lanServerHost, sizeof lanServerHost)) lanServerHost[0] = 0;
  wifiPrefs.end();
  Serial.printf("[LAN] May chu: %s\n", lanServerHost[0] ? lanServerHost : "tu tim");
  // Mang mac dinh trong secrets.h: thu sau cung, khong ghi vao NVS.
  if (strlen(WIFI_SSID)) {
    bool have = false;
    for (int i = 0; i < wifiNetCount; i++) have |= !strcmp(wifiNets[i].ssid, WIFI_SSID);
    if (!have) {
      strlcpy(wifiNets[wifiNetCount].ssid, WIFI_SSID, 33);
      strlcpy(wifiNets[wifiNetCount].pass, WIFI_PASSWORD, 65);
      wifiNetCount++;
    }
  }
  Serial.printf("[WIFI] %d mang da luu\n", wifiNetCount);
}

static void wifiPersist() {
  wifiPrefs.begin("wifi", false);
  wifiPrefs.clear();
  int n = 0;
  char key[4];
  for (int i = 0; i < wifiNetCount && n < WIFI_MAX_SAVED; i++) {
    if (!strcmp(wifiNets[i].ssid, WIFI_SSID) && !strcmp(wifiNets[i].pass, WIFI_PASSWORD) && i == wifiNetCount - 1)
      continue;  // mang mac dinh dung cuoi: khong can luu
    snprintf(key, sizeof key, "s%d", n); wifiPrefs.putString(key, wifiNets[i].ssid);
    snprintf(key, sizeof key, "p%d", n); wifiPrefs.putString(key, wifiNets[i].pass);
    n++;
  }
  wifiPrefs.putInt("n", n);
  if (lanServerHost[0]) wifiPrefs.putString("server", lanServerHost);
  wifiPrefs.end();
}

static void wifiAddFront(const char* ssid, const char* pass) {
  xSemaphoreTake(wifiLock, portMAX_DELAY);
  SavedNet fresh{};
  strlcpy(fresh.ssid, ssid, sizeof fresh.ssid);
  strlcpy(fresh.pass, pass, sizeof fresh.pass);
  int out = 1;
  SavedNet next[WIFI_MAX_SAVED + 1];
  next[0] = fresh;
  for (int i = 0; i < wifiNetCount && out < WIFI_MAX_SAVED; i++)
    if (strcmp(wifiNets[i].ssid, ssid)) next[out++] = wifiNets[i];
  memcpy(wifiNets, next, sizeof(SavedNet) * out);
  wifiNetCount = out;
  xSemaphoreGive(wifiLock);
  wifiPersist();
}

static void wifiRemove(const char* ssid) {
  xSemaphoreTake(wifiLock, portMAX_DELAY);
  int out = 0;
  for (int i = 0; i < wifiNetCount; i++)
    if (strcmp(wifiNets[i].ssid, ssid)) wifiNets[out++] = wifiNets[i];
  wifiNetCount = out;
  xSemaphoreGive(wifiLock);
  wifiPersist();
}

// ---------- Ket noi ----------

// Ly do rot mang gan nhat (bo qua ma 8 = do chinh firmware goi disconnect).
static volatile uint8_t wifiLastReason = 0;
static char wifiFailMsg[96] = "";

static bool reasonIsAuth(uint8_t r) {
  return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
         r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_AUTH_EXPIRE ||
         r == WIFI_REASON_MIC_FAILURE;
}
static bool reasonNoAp(uint8_t r) {
  return r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
         r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
         r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static void wifiSetFail(const char* ssid, uint8_t r) {
  strlcpy(wifiFail, ssid, sizeof wifiFail);
  if (reasonIsAuth(r)) strlcpy(wifiFailMsg, "Sai mật khẩu.", sizeof wifiFailMsg);
  else if (reasonNoAp(r))
    strlcpy(wifiFailMsg, "Không thấy mạng này. Tên phải đúng chữ hoa/thường và là WiFi 2.4 GHz.",
            sizeof wifiFailMsg);
  else if (!r) strlcpy(wifiFailMsg, "Router không phản hồi. Để thiết bị gần router hơn.", sizeof wifiFailMsg);
  else snprintf(wifiFailMsg, sizeof wifiFailMsg, "Kết nối thất bại (mã %u).", (unsigned)r);
  Serial.printf("[WIFI] Khong vao duoc \"%s\": ma %u - %s\n", ssid, (unsigned)r, wifiFailMsg);
}

static void wifiOnEvent(arduino_event_id_t, arduino_event_info_t info) {
  uint8_t r = info.wifi_sta_disconnected.reason;
  if (r == WIFI_REASON_ASSOC_LEAVE) return;
  wifiLastReason = r;
  Serial.printf("[WIFI] Rot/khong vao duoc, ma ly do %u\n", (unsigned)r);
}

static void wifiTryNext() {
  if (!wifiNetCount) { wifiAttempting = false; return; }
  wifiAttemptIdx = (wifiAttemptIdx + 1) % wifiNetCount;
  const SavedNet& n = wifiNets[wifiAttemptIdx];
  strlcpy(wifiSsid, n.ssid, sizeof wifiSsid);
  WiFi.disconnect(false, false);
  delay(50);
  wifiLastReason = 0;
  WiFi.begin(n.ssid, strlen(n.pass) ? n.pass : nullptr);
  wifiAttempting = true;
  wifiAttemptAt = millis();
  wifiChanged = true;
  Serial.printf("[WIFI] Thu ket noi \"%s\" (%d/%d)\n", n.ssid, wifiAttemptIdx + 1, wifiNetCount);
}

// Bat dau lai tu mang dau danh sach (vua luu / nguoi dung bam thu lai).
static void wifiRestartCycle(bool user) {
  wifiAttemptIdx = -1;
  wifiUserAttempt = user;
  wifiTryNext();
}

static void wifiStartPortal(PortalReason why) {
  portalReason = why;
  // IP cu de man "Da ket noi lai" so sanh
  if (why == PR_MANUAL && WiFi.status() == WL_CONNECTED) strlcpy(wifiOldIp, wifiIp, sizeof wifiOldIp);
  else if (why == PR_NO_WIFI) wifiOldIp[0] = 0;
  if (!apOn) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSsid);  // mang mo, giong Xiaozhi
    delay(100);
    wifiDns.setErrorReplyCode(DNSReplyCode::NoError);
    wifiDns.start(53, "*", WiFi.softAPIP());
    apOn = true;
    Serial.printf("[WIFI] Hotspot \"%s\" -> http://%s/\n", apSsid, WiFi.softAPIP().toString().c_str());
  }
  apOffAt = 0;
  wifiIdleAt = millis();
  wifiSetState(WS_PORTAL);
}

static void wifiStopPortal() {
  if (!apOn) return;
  wifiDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  apOffAt = 0;
  portalReason = PR_NONE;
  Serial.println("[WIFI] Tat hotspot");
}

static void wifiOnConnected() {
  strlcpy(wifiIp, WiFi.localIP().toString().c_str(), sizeof wifiIp);
  strlcpy(wifiSsid, WiFi.SSID().c_str(), sizeof wifiSsid);
  wifiAttempting = false; wifiUserAttempt = false;
  wifiFail[0] = 0;
  MDNS.end();
  if (MDNS.begin(DEVICE_ID)) MDNS.addService("http", "tcp", LIVE_HTTP_PORT);
  if (apOn) apOffAt = millis() + WIFI_AP_LINGER_MS;
  portalReason = PR_NONE;
  Serial.printf("[WIFI] Da ket noi \"%s\" IP %s (http://%s.local/)\n", wifiSsid, wifiIp, DEVICE_ID);
  wifiSetState(WS_ONLINE);
}

// ---------- Quet mang (chay trong loop) ----------

static void jsonEscape(String& out, const char* s) {
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) out += ' ';
    else out += c;
  }
}

static void wifiScanTick() {
  if (!scanReq) return;
  if (!scanStarted) {
    if (wifiAttempting && !wifiUserAttempt) { WiFi.disconnect(false, false); wifiAttempting = false; wifiIdleAt = millis(); }
    if (wifiAttempting) return;  // dang vao mang nguoi dung vua luu: quet sau
    int r = WiFi.scanNetworks(true, false);
    if (r == WIFI_SCAN_RUNNING) { scanStarted = true; scanBusy = true; scanStartAt = millis(); }
    return;
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING && millis() - scanStartAt < 10000) return;
  String out = "[";
  int shown = 0;
  for (int i = 0; i < n && shown < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (int j = 0; j < i; j++) dup |= WiFi.SSID(j) == ssid;  // ket qua da xep theo song manh
    if (dup) continue;
    if (shown++) out += ',';
    out += "{\"ssid\":\"";
    jsonEscape(out, ssid.c_str());
    out += "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"open\":" +
           (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  out += "]";
  WiFi.scanDelete();
  xSemaphoreTake(wifiLock, portMAX_DELAY);
  scanJson = out;
  xSemaphoreGive(wifiLock);
  scanStarted = false;
  scanBusy = false;
  scanReq = false;
}

// ---------- Trang cau hinh ----------

static const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="vi"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Cấu hình WiFi · AI Camera</title><style>
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,"Segoe UI",sans-serif;background:#f4f7fa;color:#12283a;line-height:1.45}
main{max-width:460px;margin:0 auto;padding:20px 16px 40px;display:flex;flex-direction:column;gap:16px}
h1{margin:0;font-size:22px}.dev{font-size:13px;color:#4c6478}
.lbl{font-size:11px;font-weight:700;letter-spacing:.1em;color:#4c6478;display:flex;justify-content:space-between;align-items:center}
.lbl button{border:0;background:none;color:#0b7f75;font-weight:700;font-size:13px;padding:6px}
.now{background:#fff;border:1px solid #d5e0ea;border-radius:12px;padding:12px 14px;font-size:14px}
.now b{font-size:16px}.ip{font-family:ui-monospace,Consolas,monospace;font-size:20px;font-weight:700;color:#0b7f75}
ul{list-style:none;margin:0;padding:0;background:#fff;border:1px solid #d5e0ea;border-radius:12px;overflow:hidden}
li+li{border-top:1px solid #e3ebf2}
.net{width:100%;border:0;background:none;display:flex;align-items:center;gap:10px;padding:12px;text-align:left;font:inherit;font-size:15px;color:inherit;min-height:48px;cursor:pointer}
.net span{flex:1;font-weight:600;overflow-wrap:anywhere}.net small{color:#0b7f75;font-weight:700}
.net[aria-expanded=true]{background:#e9f6f4}
.pw{padding:0 12px 12px;background:#e9f6f4;display:flex;flex-direction:column;gap:8px}
label{font-size:13px;color:#4c6478}
input{min-height:44px;border:1px solid #b9cad8;border-radius:10px;padding:0 12px;font:inherit;font-size:16px;background:#fff;color:#12283a;width:100%}
.btn{min-height:46px;border:0;border-radius:10px;background:#0b7f75;color:#fff;font:inherit;font-weight:700;font-size:15px;cursor:pointer}
.btn:disabled{opacity:.55}
.saved li{display:flex;align-items:center;justify-content:space-between;padding:10px 12px;font-size:14px;gap:8px}
.saved button{border:1px solid #d5e0ea;background:#fff;border-radius:8px;padding:6px 10px;font:inherit;font-size:13px;color:#b3261e;cursor:pointer}
.msg{border-radius:12px;padding:12px 14px;font-size:14px}.msg b{display:block;font-size:15px}
.ok{background:#e2f5ea;border:1px solid #9fd8b6;color:#14532d}.err{background:#fdecec;border:1px solid #f3b4b4;color:#7f1d1d}.wait{background:#fff7e0;border:1px solid #f0d58a;color:#6b4500}
.hint{font-size:12px;color:#4c6478;margin:0}.empty{padding:12px;font-size:14px;color:#4c6478}
svg{flex:none}
</style></head><body><main>
<div><h1>Cấu hình WiFi</h1><div class="dev" id="dev">AI Camera</div></div>
<div class="now" id="now">Đang đọc trạng thái…</div>
<div id="msg" class="msg" hidden></div>
<div class="lbl">MẠNG XUNG QUANH <button id="rescan" type="button">Quét lại</button></div>
<ul id="nets"><li class="empty">Đang quét…</li></ul>
<div class="lbl">MẠNG KHÁC (ẨN)</div>
<ul><li><button class="net" id="hidden-btn" type="button" aria-expanded="false"><span>Nhập tên mạng</span></button></li></ul>
<div class="lbl">ĐÃ LƯU · THỬ THEO THỨ TỰ</div>
<ul class="saved" id="saved"></ul>
<p class="hint">Mật khẩu chỉ lưu trong thiết bị, không gửi lên Cloud. Lưu tối đa 5 mạng.</p>
<div class="lbl">MÁY CHỦ NHẬN DIỆN (PI HOẶC MÁY TÍNH)</div>
<div class="now"><label for="srv">IP máy chạy server, cổng 8000. Để trống: tự tìm ai-camera.local, rồi thử gateway.</label>
<div style="display:flex;gap:8px;margin-top:8px"><input id="srv" maxlength="39" placeholder="vd 192.168.137.1" autocomplete="off" inputmode="decimal"><button class="btn" id="srv-save" type="button" style="padding:0 16px">Lưu</button></div>
<p class="hint" id="srv-note" style="margin-top:6px"></p></div>
</main><script>
const $=id=>document.getElementById(id);let open=null,waitFor=null,waitAt=0,last=null;
const bars=r=>{const lv=r>-55?4:r>-67?3:r>-78?2:1;let s='<svg width="18" height="18" viewBox="0 0 24 24">';for(let i=0;i<4;i++)s+=`<rect x="${3+i*5}" y="${18-i*4}" width="3" height="${4+i*4}" rx="1" fill="${i<lv?'#0b7f75':'#c9d7e3'}"/>`;return s+'</svg>'};
const esc=s=>s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function msg(cls,html){const m=$('msg');m.className='msg '+cls;m.innerHTML=html;m.hidden=!html}
function form(ssid,isOpen,hidden){return `<div class="pw">${hidden?'<label for="f-ssid">Tên mạng (SSID)</label><input id="f-ssid" maxlength="32" autocomplete="off">':''}${isOpen?'':'<label for="f-pass">Mật khẩu'+(hidden?'':' "'+esc(ssid)+'"')+'</label><input id="f-pass" type="password" maxlength="63" autocomplete="off">'}<button class="btn" id="f-save" type="button">Lưu và kết nối</button></div>`}
function renderNets(list,saved){const ul=$('nets');if(!list.length){ul.innerHTML='<li class="empty">Không thấy mạng nào. Bấm Quét lại.</li>';return}
ul.innerHTML=list.map((n,i)=>`<li><button class="net" type="button" data-i="${i}" aria-expanded="${open===n.ssid}">${bars(n.rssi)}<span>${esc(n.ssid)}</span>${saved.includes(n.ssid)?'<small>Đã lưu</small>':''}${n.open?'':'<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="#4c6478" stroke-width="2"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/></svg>'}</button>${open===n.ssid?form(n.ssid,n.open,false):''}</li>`).join('');
ul.querySelectorAll('.net').forEach(b=>b.onclick=()=>{const n=list[+b.dataset.i];open=open===n.ssid?null:n.ssid;renderNets(list,saved);bindSave(n.ssid,n.open);const p=$('f-pass');if(p)p.focus()});bindSave(open,(list.find(n=>n.ssid===open)||{}).open)}
function bindSave(ssid,isOpen){const b=$('f-save');if(!b)return;b.onclick=async()=>{const s=$('f-ssid')?$('f-ssid').value.trim():ssid;const p=$('f-pass')?$('f-pass').value:'';
if(!s){msg('err','<b>Thiếu tên mạng</b>Nhập tên WiFi cần kết nối.');return}
if(!isOpen&&p.length&&p.length<8){msg('err','<b>Mật khẩu quá ngắn</b>Mật khẩu WiFi cần ít nhất 8 ký tự.');return}
b.disabled=true;b.textContent='Đang lưu…';
try{const r=await fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)});if(!r.ok)throw new Error(await r.text());
waitFor=s;waitAt=Date.now();open=null;msg('wait','<b>Đang kết nối "'+esc(s)+'"…</b>Chờ khoảng 15 giây. Đừng tắt trang này.')}catch(e){msg('err','<b>Không lưu được</b>'+esc(String(e.message||e)));b.disabled=false;b.textContent='Lưu và kết nối'}}}
$('hidden-btn').onclick=e=>{const li=e.currentTarget.parentNode;const ex=li.querySelector('.pw');if(ex){ex.remove();e.currentTarget.setAttribute('aria-expanded','false');return}e.currentTarget.setAttribute('aria-expanded','true');li.insertAdjacentHTML('beforeend',form('',false,true));bindSave('',false);$('f-ssid').focus()};
async function forget(s){await fetch('/api/wifi/forget',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)});poll()}
var poll=async function(scan){try{const d=await (await fetch('/api/wifi'+(scan?'?scan=1':''),{cache:'no-store'})).json();last=d;
$('dev').textContent='AI Camera · '+d.device+(d.ap?' · hotspot '+d.ap_ssid:'');
$('now').innerHTML=d.state==='online'?`Đang dùng <b>${esc(d.ssid)}</b><div class="ip">${d.ip}</div>Trong cùng mạng: http://${d.ip}/ hoặc http://${d.device}.local/`:d.trying?`Đang thử kết nối <b>${esc(d.trying)}</b>…`:'Chưa kết nối WiFi nào.';
if(!document.querySelector('.pw'))renderNets(d.nets,d.saved);
$('saved').innerHTML=d.saved.length?d.saved.map((s,i)=>`<li><span>${i+1}. ${esc(s)}</span><button type="button" data-s="${esc(s)}">Quên</button></li>`).join(''):'<li class="empty">Chưa lưu mạng nào.</li>';
$('saved').querySelectorAll('button').forEach(b=>b.onclick=()=>forget(b.dataset.s));
if(waitFor){if(d.state==='online'&&d.ssid===waitFor){msg('ok',`<b>Đã kết nối "${esc(waitFor)}"</b>Địa chỉ mới: <span class="ip">${d.ip}</span><br>Hotspot sẽ tắt sau ít giây. Nối điện thoại vào "${esc(waitFor)}" rồi mở http://${d.ip}/ hoặc http://${d.device}.local/`);waitFor=null}
else if(d.fail===waitFor||Date.now()-waitAt>60000){msg('err',`<b>Không vào được "${esc(waitFor)}"</b>${esc(d.fail_msg||'Thiết bị chưa vào được mạng. Thử lại hoặc để gần router hơn.')}`);waitFor=null}}
}catch(e){if(waitFor)msg('wait','<b>Đang kết nối "'+esc(waitFor)+'"…</b>Điện thoại có thể bị ngắt khỏi hotspot vài giây. Trang sẽ tự cập nhật.')}}
let srvInit=false;function srvNote(d){$('srv-note').textContent=(d.server?'Đang dùng: '+d.server:'Đang tự tìm máy chủ')+(d.gateway?' · gateway mạng này: '+d.gateway:'')}
const _poll=poll;poll=async s=>{await _poll(s);if(last){if(!srvInit){$('srv').value=last.server||'';srvInit=true}srvNote(last)}};
$('srv-save').onclick=async()=>{const b=$('srv-save');b.disabled=true;try{const r=await fetch('/api/server',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'host='+encodeURIComponent($('srv').value.trim())});if(!r.ok)throw new Error(await r.text());msg('ok','<b>Đã lưu máy chủ</b>Lần chụp tiếp theo sẽ gửi ảnh tới đây.')}catch(e){msg('err','<b>Không lưu được</b>'+esc(String(e.message||e)))}b.disabled=false;setTimeout(()=>poll(false),800)};
$('rescan').onclick=()=>{$('nets').innerHTML='<li class="empty">Đang quét…</li>';open=null;poll(true)};
poll(true);setInterval(()=>poll(false),2500);
</script></body></html>)HTML";

static void urlDecode(char* s) {
  char* o = s;
  for (; *s; s++) {
    if (*s == '+') *o++ = ' ';
    else if (*s == '%' && isxdigit((uint8_t)s[1]) && isxdigit((uint8_t)s[2])) {
      char h[3] = {s[1], s[2], 0};
      *o++ = (char)strtol(h, nullptr, 16);
      s += 2;
    } else *o++ = *s;
  }
  *o = 0;
}

// Doc than POST dang a=1&b=2 (toi da 512 byte).
static bool readForm(httpd_req_t* req, char* body, size_t cap) {
  if (req->content_len >= cap) return false;
  int got = 0;
  while (got < (int)req->content_len) {
    int r = httpd_req_recv(req, body + got, req->content_len - got);
    if (r <= 0) return false;
    got += r;
  }
  body[got] = 0;
  return true;
}

static bool formField(const char* body, const char* key, char* out, size_t cap) {
  char raw[200];
  if (httpd_query_key_value(body, key, raw, sizeof raw) != ESP_OK) { out[0] = 0; return false; }
  urlDecode(raw);
  if (strlen(raw) >= cap) return false;
  strlcpy(out, raw, cap);
  return true;
}

static esp_err_t sendText(httpd_req_t* req, const char* status, const char* body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portalPage(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portalStatus(httpd_req_t* req) {
  char q[16];
  if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "scan=1")) scanReq = true;
  const char* st = wifiState == WS_ONLINE ? "online" : wifiState == WS_LOST ? "lost"
                 : wifiState == WS_PORTAL ? "portal" : "connecting";
  bool up = WiFi.status() == WL_CONNECTED;
  String out = "{\"device\":\"";
  out += DEVICE_ID;
  out += "\",\"state\":\"";
  out += up && wifiState == WS_PORTAL ? "online" : st;
  out += "\",\"ssid\":\"";
  jsonEscape(out, up ? wifiSsid : "");
  out += "\",\"ip\":\"";
  out += up ? wifiIp : "";
  out += "\",\"trying\":\"";
  if (wifiAttempting) jsonEscape(out, wifiSsid);
  out += "\",\"fail\":\"";
  jsonEscape(out, wifiFail);
  out += "\",\"fail_msg\":\"";
  jsonEscape(out, wifiFailMsg);
  out += "\",\"server\":\"";
  jsonEscape(out, lanServerHost);
  out += "\",\"gateway\":\"";
  out += up ? WiFi.gatewayIP().toString() : String("");
  out += "\",\"ap\":";
  out += apOn ? "true" : "false";
  out += ",\"ap_ssid\":\"";
  out += apSsid;
  out += "\",\"scanning\":";
  out += scanBusy || scanReq ? "true" : "false";
  out += ",\"saved\":[";
  xSemaphoreTake(wifiLock, portMAX_DELAY);
  for (int i = 0; i < wifiNetCount; i++) {
    if (i) out += ',';
    out += '"';
    jsonEscape(out, wifiNets[i].ssid);
    out += '"';
  }
  out += "],\"nets\":";
  out += scanJson;
  xSemaphoreGive(wifiLock);
  out += "}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out.c_str(), out.length());
}

static esp_err_t portalSave(httpd_req_t* req) {
  char body[512], ssid[33], pass[65];
  if (!readForm(req, body, sizeof body)) return sendText(req, "400 Bad Request", "Dữ liệu không hợp lệ.");
  if (!formField(body, "ssid", ssid, sizeof ssid) || !strlen(ssid))
    return sendText(req, "400 Bad Request", "Tên mạng phải từ 1 đến 32 ký tự.");
  formField(body, "pass", pass, sizeof pass);
  size_t pl = strlen(pass);
  if (pl && (pl < 8 || pl > 63)) return sendText(req, "400 Bad Request", "Mật khẩu phải từ 8 đến 63 ký tự.");
  if (pendSave) return sendText(req, "409 Conflict", "Đang xử lý lần lưu trước, thử lại sau vài giây.");
  strlcpy(pendSsid, ssid, sizeof pendSsid);
  strlcpy(pendPass, pass, sizeof pendPass);
  pendSave = true;
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portalServer(httpd_req_t* req) {
  char body[200], host[40];
  if (!readForm(req, body, sizeof body)) return sendText(req, "400 Bad Request", "Dữ liệu không hợp lệ.");
  if (!formField(body, "host", host, sizeof host)) host[0] = 0;
  for (char* c = host; *c; c++)
    if (!isalnum((uint8_t)*c) && *c != '.' && *c != '-')
      return sendText(req, "400 Bad Request", "Chỉ nhập IP (vd 192.168.137.1) hoặc tên máy, không kèm http:// hay cổng.");
  strlcpy(pendServer, host, sizeof pendServer);
  pendServerReq = true;
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portalForget(httpd_req_t* req) {
  char body[200], ssid[33];
  if (!readForm(req, body, sizeof body) || !formField(body, "ssid", ssid, sizeof ssid))
    return sendText(req, "400 Bad Request", "Thiếu tên mạng.");
  strlcpy(pendForget, ssid, sizeof pendForget);
  pendForgetReq = true;
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// Moi duong dan la (generate_204, hotspot-detect.html...) -> chuyen ve trang cau hinh.
static esp_err_t portalRedirect(httpd_req_t* req, httpd_err_code_t) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", apOn ? "http://192.168.4.1/" : "/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, nullptr, 0);
}

static void portalRoutes(httpd_handle_t s) {
  static const httpd_uri_t routes[] = {
    {"/", HTTP_GET, portalPage, nullptr},
    {"/api/wifi", HTTP_GET, portalStatus, nullptr},
    {"/api/wifi/save", HTTP_POST, portalSave, nullptr},
    {"/api/wifi/forget", HTTP_POST, portalForget, nullptr},
    {"/api/server", HTTP_POST, portalServer, nullptr},
  };
  for (const auto& r : routes) httpd_register_uri_handler(s, &r);
  httpd_register_err_handler(s, HTTPD_404_NOT_FOUND, portalRedirect);
}

// ---------- API cho firmware ----------

static void wifiBegin(bool forcePortal) {
  if (!wifiLock) wifiLock = xSemaphoreCreateMutex();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_ID);
  WiFi.setSleep(false);  // giam tre stream
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(wifiOnEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof apSsid, "AI-Camera-%02X%02X", mac[4], mac[5]);
  wifiLoad();
  liveRegisterExtra = portalRoutes;
  liveStartHttp();  // HTTP nghe tren ca mang WiFi va hotspot
  if (forcePortal || !wifiNetCount) {
    wifiStartPortal(forcePortal ? PR_MANUAL : PR_NO_WIFI);
    if (wifiNetCount) wifiRestartCycle(false);
  } else {
    wifiSetState(WS_CONNECTING);
    wifiRestartCycle(false);
  }
}

// Nguoi dung giu BACK 3 s: mo hotspot de doi WiFi (van giu mang hien tai neu co).
static void wifiOpenPortal(PortalReason why) { wifiStartPortal(why); }

// Thoat trang cau hinh khi dang co mang.
static void wifiClosePortal() {
  wifiStopPortal();
  if (WiFi.status() == WL_CONNECTED) wifiOnConnected();
}

static void wifiRetryNow() { wifiRestartCycle(true); }

static void wifiTick() {
  uint32_t now = millis();
  if (apOn) wifiDns.processNextRequest();

  // Dang quet mang thi WiFi.begin() bi tu choi -> doi quet xong moi ket noi.
  if (pendSave && !scanStarted) {
    Serial.printf("[WIFI] Luu mang \"%s\" (mat khau %u ky tu)\n", pendSsid, (unsigned)strlen(pendPass));
    wifiAddFront(pendSsid, pendPass);
    wifiFail[0] = 0;
    wifiFailMsg[0] = 0;
    if (wifiState != WS_PORTAL) wifiStartPortal(PR_MANUAL);  // luu tu trang trong mang: giu hotspot de bao loi
    pendSave = false;
    wifiRestartCycle(true);
  }
  if (pendServerReq) {
    strlcpy(lanServerHost, pendServer, sizeof lanServerHost);
    pendServerReq = false;
    wifiPrefs.begin("wifi", false);
    if (lanServerHost[0]) wifiPrefs.putString("server", lanServerHost);
    else wifiPrefs.remove("server");
    wifiPrefs.end();
    lanServerGen = lanServerGen + 1;
    Serial.printf("[LAN] Doi may chu: %s\n", lanServerHost[0] ? lanServerHost : "tu tim");
  }
  if (pendForgetReq) {
    Serial.printf("[WIFI] Quen mang \"%s\"\n", pendForget);
    wifiRemove(pendForget);
    pendForgetReq = false;
  }
  wifiScanTick();

  bool up = WiFi.status() == WL_CONNECTED;
  switch (wifiState) {
    case WS_CONNECTING:
      if (up) { wifiOnConnected(); break; }
      if (wifiAttempting && now - wifiAttemptAt > WIFI_ATTEMPT_MS) {
        if (wifiAttemptIdx >= wifiNetCount - 1) {
          Serial.println("[WIFI] Khong vao duoc mang nao -> bat hotspot");
          WiFi.disconnect(false, false);
          wifiAttempting = false;
          wifiStartPortal(PR_NO_WIFI);
        } else wifiTryNext();
      }
      break;

    case WS_ONLINE:
      if (apOn && apOffAt && (int32_t)(now - apOffAt) >= 0) wifiStopPortal();
      if (up) { wifiLostSince = 0; break; }
      if (!wifiLostSince) wifiLostSince = now;
      else if (now - wifiLostSince > 2000) {
        strlcpy(wifiOldIp, wifiIp, sizeof wifiOldIp);
        wifiLostSince = 0;
        Serial.printf("[WIFI] Mat ket noi \"%s\"\n", wifiSsid);
        wifiSetState(WS_LOST);  // WiFi tu ket noi lai mang cu (setAutoReconnect)
      }
      break;

    case WS_LOST:
      if (up) { wifiOnConnected(); break; }
      if (now - wifiStateAt > WIFI_LOST_TO_AP_MS) wifiStartPortal(PR_LOST);
      break;

    case WS_PORTAL: {
      // Dang o mang cu ma nguoi dung chu dong mo hotspot: chi thoat khi vao mang moi.
      if (up && (wifiAttempting || portalReason != PR_MANUAL)) { wifiOnConnected(); break; }
      if (up || !wifiNetCount) break;
      bool phone = WiFi.softAPgetStationNum() > 0;
      // Mang vua luu: cho lau hon (hotspot + doi kenh cham), nhung bao sai mat khau /
      // khong thay mang som khi ESP32 da tra ma loi ro rang.
      uint32_t limit = wifiUserAttempt ? WIFI_USER_ATTEMPT_MS : WIFI_ATTEMPT_MS;
      uint8_t reason = wifiLastReason;
      bool clearFail = wifiUserAttempt && now - wifiAttemptAt > 8000 &&
                       (reasonIsAuth(reason) || reasonNoAp(reason));
      if (wifiAttempting && (clearFail || now - wifiAttemptAt > limit)) {
        if (wifiUserAttempt) wifiSetFail(wifiSsid, reason);
        WiFi.disconnect(false, false);
        wifiAttempting = false; wifiUserAttempt = false;
        wifiIdleAt = now;
        wifiChanged = true;
      }
      // Dien thoai dang noi hotspot: tam dung do mang de hotspot khong bi chap chon.
      if (phone && wifiAttempting && !wifiUserAttempt) {
        WiFi.disconnect(false, false);
        wifiAttempting = false;
        wifiIdleAt = now;
        wifiChanged = true;
      }
      if (!wifiAttempting && !phone && now - wifiIdleAt > WIFI_PORTAL_RETRY_MS) wifiTryNext();
      break;
    }
  }
}
