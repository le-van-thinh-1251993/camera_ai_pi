import json, time, base64, urllib.request
from pathlib import Path
import websocket
import argparse
from dotenv import dotenv_values

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description="Chrome headless + real Supabase camera smoke test")
parser.add_argument("--cdp", default="http://127.0.0.1:9237")
parser.add_argument("--url", default="http://127.0.0.1:8000/")
parser.add_argument("--key-env", type=Path, default=project / ".env")
args = parser.parse_args()
key = dotenv_values(args.key_env).get("API_KEY")
if not key:
    raise SystemExit("Missing dashboard API_KEY; never pass Supabase secret keys to this test")
root = project
output = project / "data/browser-qa"
output.mkdir(parents=True, exist_ok=True)
page = next(
    t
    for t in json.load(urllib.request.urlopen(args.cdp.rstrip("/") + "/json"))
    if t["type"] == "page"
)
ws = websocket.create_connection(page["webSocketDebuggerUrl"], timeout=30, origin=args.cdp)
counter = 0
errors = []
requests = []


def call(method, params=None):
    global counter
    counter += 1
    ident = counter
    ws.send(json.dumps({"id": ident, "method": method, "params": params or {}}))
    while True:
        result = json.loads(ws.recv())
        if result.get("method") == "Runtime.exceptionThrown":
            errors.append(result["params"])
        if result.get("method") == "Network.requestWillBeSent":
            requests.append(result["params"])
        if result.get("id") == ident:
            if "error" in result:
                raise RuntimeError(result["error"])
            return result.get("result", {})


def evaluate(code):
    result = call(
        "Runtime.evaluate", {"expression": code, "awaitPromise": True, "returnByValue": True}
    )
    if "exceptionDetails" in result:
        raise RuntimeError(result["exceptionDetails"])
    return result.get("result", {}).get("value")


def until(code, timeout=100):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if evaluate(code):
            return
        time.sleep(0.3)
    raise AssertionError(
        "Timeout: " + code + "; " + str(evaluate("document.getElementById('error')?.textContent"))
    )


def shot(name):
    data = call("Page.captureScreenshot", {"format": "png"})["data"]
    (output / name).write_bytes(base64.b64decode(data))


call("Runtime.enable")
call("Network.enable")
call("Page.enable")
call("Network.setCacheDisabled", {"cacheDisabled": True})
call(
    "Emulation.setDeviceMetricsOverride",
    {"width": 1366, "height": 768, "deviceScaleFactor": 1, "mobile": False},
)
call("Page.navigate", {"url": args.url})
until("document.getElementById('settings-dialog')?.open")
evaluate(
    "document.getElementById('key').value="
    + json.dumps(key)
    + ";document.getElementById('connect').click()"
)
until("!document.getElementById('settings-dialog').open")
evaluate(
    "window.audioPlays=0;window.originalPlay=HTMLMediaElement.prototype.play;HTMLMediaElement.prototype.play=function(){if(this.tagName==='AUDIO')audioPlays++;return originalPlay.apply(this,arguments);}"
)
doc = call("DOM.getDocument")
node = call("DOM.querySelector", {"nodeId": doc["root"]["nodeId"], "selector": "#file"})["nodeId"]
call("DOM.setFileInputFiles", {"nodeId": node, "files": [str(project / "samples/victory.jpg")]})
evaluate("document.getElementById('form').requestSubmit()")
until(
    "document.getElementById('gesture').textContent==='Phát hiện bạn đang giơ 2 ngón tay' && audioPlays===1"
)
time.sleep(2)
assert evaluate("audioPlays") == 1
shot("qa-upload.png")
report = {
    "upload_one_voice": True,
    "upload_height": evaluate("document.documentElement.scrollHeight"),
}
evaluate("""(async()=>{
 window.sample=new Image();sample.src=document.getElementById('photo').src;await sample.decode();
 window.feed=document.createElement('canvas');feed.width=sample.naturalWidth;feed.height=sample.naturalHeight;
 window.feedInterval=setInterval(()=>feed.getContext('2d').drawImage(sample,0,0),80);
 navigator.mediaDevices.getUserMedia=async()=>feed.captureStream(12);
 document.getElementById('tab-camera').click();document.getElementById('camera-start').click();
})()""")
until("!document.getElementById('resume-round').hidden && audioPlays===2")
camera_posts = [
    r
    for r in requests
    if r["request"]["url"].endswith("/api/camera") and r["request"]["method"] == "POST"
]
assert len(camera_posts) == 1, len(camera_posts)
assert evaluate("document.querySelector('#live-preview').hidden")
time.sleep(2)
assert evaluate("audioPlays") == 2
report.update(
    auto_cloud_confirmed_stopped=True,
    auto_camera_posts=1,
    phone_height=evaluate("document.documentElement.scrollHeight"),
)
shot("qa-auto.png")
evaluate(
    "document.getElementById('phone-mode').value='manual';document.getElementById('phone-mode').dispatchEvent(new Event('change'));document.getElementById('camera-start').click()"
)
until("!document.getElementById('live-preview').hidden")
evaluate(
    "document.getElementById('phone-freeze').click();document.getElementById('phone-capture').click()"
)
until("!document.getElementById('resume-round').hidden && audioPlays===3")
report["manual_cloud_confirmed_stopped"] = True
evaluate("clearInterval(feedInterval);document.getElementById('theme').click()")
shot("qa-light.png")
call(
    "Emulation.setDeviceMetricsOverride",
    {"width": 390, "height": 844, "deviceScaleFactor": 1, "mobile": True},
)
time.sleep(0.5)
shot("qa-mobile.png")
report["mobile_width"] = evaluate("document.documentElement.scrollWidth")
assert report["mobile_width"] == 390
report["js_errors"] = len(errors)
assert not errors, errors
(output / "ui-report.json").write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
ws.close()
