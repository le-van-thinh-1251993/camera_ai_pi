"""Authenticated dashboard; all public image/camera endpoints enqueue work in Supabase."""

import asyncio, json, logging, os, secrets, sqlite3, time, uuid
from contextlib import asynccontextmanager, closing, suppress
from datetime import datetime, timezone
from logging.handlers import RotatingFileHandler
from pathlib import Path
from dotenv import load_dotenv
from fastapi import Depends, FastAPI, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

BASE = Path(__file__).resolve().parents[1]
# Bien moi truong dat san (run_local_lan.ps1, systemd) thang .env.
load_dotenv(BASE / ".env", override=False)
load_dotenv(BASE / ".env.cloud", override=False)
from app.vision_engine import Engine
from app.cloud_worker import CloudRuntime
from app.cloud_web import attach_cloud_web
from app.cloud_control import attach_cloud_control
from app.lan_web import attach_lan_web
from app.lan_discovery import start_discovery
from app.esp32_live import attach_esp32_live, note_device_ip
from app.stats import compute_stats

DATA = Path(os.getenv("DATA_DIR", str(BASE / "data"))).resolve()
for directory in (DATA, DATA / "audio", DATA / "logs", DATA / "hybrid"):
    directory.mkdir(parents=True, exist_ok=True)
logging.basicConfig(
    level=logging.INFO,
    handlers=[
        logging.StreamHandler(),
        RotatingFileHandler(
            DATA / "logs/app.log", maxBytes=2_000_000, backupCount=3, encoding="utf-8"
        ),
    ],
)
log = logging.getLogger("ai-camera")
logging.getLogger("httpx").setLevel(logging.WARNING)
KEY = os.getenv("API_KEY", "").strip()
LAN_KEY = os.getenv("LAN_API_KEY", "").strip() or KEY
PHRASES = json.loads((BASE / "config/gestures.json").read_text(encoding="utf-8"))
engine = None
gate = asyncio.Lock()
listeners = set()
last_event = None


async def auth(request: Request):
    if not KEY:
        return  # Testing mode: skip auth when API_KEY is empty
    if not secrets.compare_digest(request.headers.get("X-API-Key", ""), KEY):
        raise HTTPException(
            401, "Khóa dashboard chưa đúng. Mở Cài đặt kết nối để nhập API_KEY trên Pi."
        )


def save(event):
    with closing(sqlite3.connect(DATA / "history.db")) as db, db:
        if db.execute(
            "SELECT 1 FROM events WHERE json_extract(payload,'$.id')=? LIMIT 1", (event["id"],)
        ).fetchone():
            return False
        db.execute(
            "INSERT INTO events(payload) VALUES (?)", (json.dumps(event, ensure_ascii=False),)
        )
        db.execute(
            "DELETE FROM events WHERE id NOT IN (SELECT id FROM events ORDER BY id DESC LIMIT ?)",
            (max(1, min(50, int(os.getenv("HISTORY_LIMIT", "50")))),),
        )
        return True


async def commit(event):
    global last_event
    if not await asyncio.to_thread(save, event):
        return
    last_event = event
    for queue in tuple(listeners):
        if queue.full():
            queue.get_nowait()
        queue.put_nowait(event)


async def voice(key):
    from app.tts_service import generate

    return await generate(
        key, PHRASES.get(key, PHRASES.get("Unknown", "Chưa nhận diện được cử chỉ")), DATA
    )


async def recognize(raw, source, focus=None, count_all=False, request_id=None):
    """Private worker callback. No HTTP route calls MediaPipe directly."""
    if not engine or not engine.rec:
        raise HTTPException(503, "Model chưa sẵn sàng")
    if not (raw.startswith(b"\xff\xd8") or raw.startswith(b"\x89PNG\r\n\x1a\n")):
        raise HTTPException(415, "Chỉ nhận JPEG/PNG")
    if gate.locked():
        raise HTTPException(429, "Đang xử lý tác vụ trước")
    async with gate:
        started = time.monotonic()
        try:
            hands, best, jpg = await asyncio.to_thread(
                engine.run, raw, "upload" if source == "upload" else "browser", focus, count_all
            )
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from None
        rid = str(uuid.UUID(request_id)) if request_id else str(uuid.uuid4())
        tmp = DATA / "hybrid" / (rid + ".preview.tmp")
        tmp.write_bytes(jpg)
        tmp.replace(DATA / "hybrid" / (rid + ".preview.jpg"))
        return {
            "id": rid,
            "source": source,
            "time": datetime.now(timezone.utc).isoformat(),
            "gesture": best["gesture"],
            "finger_count": best.get("finger_count"),
            "score": best.get("score"),
            "method": "landmark_count",
            "partial": bool(best.get("partial", False)),
            "visible_finger_count": best.get("visible_finger_count"),
            "text": PHRASES.get(
                best["gesture"], PHRASES.get("Unknown", "Chưa nhận diện được cử chỉ")
            ),
            "hands": hands,
            "latency_ms": round((time.monotonic() - started) * 1000),
        }


@asynccontextmanager
async def lifespan(app):
    global engine
    open_lan = os.getenv("ALLOW_OPEN_DASHBOARD", "") == "1"  # chi de test local (run_local_lan.ps1)
    if not KEY and open_lan:
        log.warning(
            "Dashboard KHÔNG có khóa (ALLOW_OPEN_DASHBOARD=1) - ai cùng mạng LAN cũng mở được."
        )
    if (
        not KEY
        and not open_lan
        and os.getenv("HOST", "0.0.0.0") not in ("127.0.0.1", "localhost", "::1")
    ):
        raise RuntimeError(
            "API_KEY không được để trống khi mở dashboard ngoài loopback. Chạy install_pi.sh để tạo khóa nếu chưa có."
        )
    with closing(sqlite3.connect(DATA / "history.db")) as db, db:
        db.execute(
            "CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY,payload TEXT NOT NULL)"
        )
        db.execute(
            "DELETE FROM events WHERE id NOT IN (SELECT id FROM events ORDER BY id DESC LIMIT 50)"
        )
    engine = await asyncio.to_thread(Engine)
    app.state.cloud_runtime = CloudRuntime(DATA, recognize, commit, voice)
    app.state.lan_api_key = LAN_KEY
    task = asyncio.create_task(app.state.cloud_runtime.run())
    discovery = await start_discovery(int(os.getenv("PORT", "8000")), note_device_ip)
    try:
        yield
    finally:
        if discovery:
            discovery.close()
        task.cancel()
        with suppress(asyncio.CancelledError):
            await task
        if engine and engine.rec:
            engine.rec.close()


app = FastAPI(
    title="Gesture to Speech Cloud",
    lifespan=lifespan,
    docs_url=None,
    redoc_url=None,
    openapi_url=None,
)
app.mount("/static", StaticFiles(directory=BASE / "static"), name="static")


class TooLarge(Exception):
    pass


class BodyLimit:
    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http" or scope.get("method") not in ("POST", "PUT", "PATCH"):
            return await self.app(scope, receive, send)
        limit = int(os.getenv("MAX_PHOTO_MB", "15")) * 1024 * 1024 + 65536
        total = 0
        started = False
        headers = dict(scope.get("headers", []))
        length = headers.get(b"content-length", b"")
        if length and (not length.isdigit() or int(length) > limit):
            return await JSONResponse({"detail": "Dữ liệu quá lớn"}, 413)(scope, receive, send)

        async def limited():
            nonlocal total
            message = await receive()
            total += len(message.get("body", b""))
            if total > limit:
                raise TooLarge()
            return message

        async def tracked(message):
            nonlocal started
            if message["type"] == "http.response.start":
                started = True
            await send(message)

        try:
            await self.app(scope, limited, tracked)
        except TooLarge:
            if started:
                raise
            await JSONResponse({"detail": "Dữ liệu quá lớn"}, 413)(scope, receive, send)


app.add_middleware(BodyLimit)


@app.get("/")
async def index():
    return FileResponse(BASE / "static/index.html", headers={"Cache-Control": "no-cache"})


@app.get("/health")
async def health(request: Request):
    # ESP32 goi /health dinh ky: ghi nho IP cua no de chuyen tiep live camera.
    if request.client and request.headers.get("user-agent", "").startswith("ESP32HTTPClient"):
        note_device_ip(request.client.host)
    rt = getattr(app.state, "cloud_runtime", None)
    ready = bool(engine and engine.rec)
    return JSONResponse(
        {
            "status": "ok" if ready else "starting",
            "ready": ready,
            "cloud_ready": bool(rt and rt.state["connected"]),
            "mode": (
                "local-only"
                if os.getenv("LOCAL_ONLY", "0") == "1"
                else "supabase-with-local-fallback"
            ),
        },
        status_code=200 if ready else 503,
    )


@app.get("/api/diagnostics", dependencies=[Depends(auth)])
async def diagnostics():
    import cv2, mediapipe

    rt = app.state.cloud_runtime
    return {
        "ready": bool(engine and engine.rec),
        "mediapipe": mediapipe.__version__,
        "opencv": cv2.__version__,
        "cloud": rt.state,
        "hold_seconds": 2,
        "pipeline": 6,
    }


@app.get("/api/history", dependencies=[Depends(auth)])
async def history(limit: int = 50):
    with closing(sqlite3.connect(DATA / "history.db")) as db:
        return [
            json.loads(row[0])
            for row in db.execute(
                "SELECT payload FROM events ORDER BY id DESC LIMIT ?",
                (max(1, min(limit, 50)),),
            )
        ]


@app.get("/api/stats", dependencies=[Depends(auth)])
async def stats(range_name: str = Query("today", alias="range")):
    with closing(sqlite3.connect(DATA / "history.db")) as db:
        events = [
            json.loads(row[0]) for row in db.execute("SELECT payload FROM events ORDER BY id")
        ]
    try:
        return compute_stats(events, range_name)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from None


@app.get("/api/audio/{name}", dependencies=[Depends(auth)])
async def audio(name: str):
    if (
        len(name) != 68
        or not name.endswith(".wav")
        or any(c not in "0123456789abcdef" for c in name[:-4])
    ):
        raise HTTPException(404)
    path = DATA / "audio" / name
    if not path.is_file():
        raise HTTPException(404)
    return FileResponse(path, media_type="audio/wav")


@app.get("/api/greeting", dependencies=[Depends(auth)])
async def greeting():
    url, kind = await voice("Greeting")
    return {
        "id": "greeting",
        "greeting": True,
        "text": PHRASES.get("Greeting", "Xin chào!"),
        "audio_url": url,
        "tts": kind,
    }


@app.get("/api/events", dependencies=[Depends(auth)])
async def events(request: Request):
    if len(listeners) >= 20:
        raise HTTPException(429, "Quá nhiều kết nối dashboard")

    async def stream():
        queue = asyncio.Queue(maxsize=4)
        listeners.add(queue)
        try:
            if last_event:
                yield "data: " + json.dumps(last_event, ensure_ascii=False) + "\n\n"
            while not await request.is_disconnected():
                try:
                    event = await asyncio.wait_for(queue.get(), 15)
                    yield "data: " + json.dumps(event, ensure_ascii=False) + "\n\n"
                except asyncio.TimeoutError:
                    yield ": ping\n\n"
        finally:
            listeners.discard(queue)

    return StreamingResponse(
        stream(),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.api_route("/api/frame", methods=["POST"], dependencies=[Depends(auth)])
@app.api_route("/api/hybrid/{path:path}", methods=["GET", "POST"], dependencies=[Depends(auth)])
@app.api_route("/api/device/{path:path}", methods=["GET", "POST"], dependencies=[Depends(auth)])
async def retired(path: str = ""):
    raise HTTPException(410, "Đường nhận diện LAN cũ đã tắt. Dùng Supabase Cloud và giao diện v6.")


attach_cloud_web(app, auth)
attach_cloud_control(app, auth)
attach_lan_web(app)
attach_esp32_live(app, KEY)
