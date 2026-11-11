"""Relay the ESP32 MJPEG live stream through the Pi.

The ESP32 serves ``http://<host>:81/stream`` (multipart JPEG) and
``http://<host>/status`` on the LAN. Relaying through the Pi lets the dashboard
show it over the same origin (including the HTTPS ngrok link, where the browser
would block a plain-HTTP ESP32 URL) and keeps the ESP32 to one upstream client.
"""

import asyncio
import logging
import os
import secrets

import httpx
from fastapi import HTTPException, Request
from fastapi.responses import JSONResponse, StreamingResponse

log = logging.getLogger("ai-camera.esp32-live")
_HOST_ENV = os.getenv("ESP32_LIVE_HOST", "").strip()
HOST = _HOST_ENV or "esp32-01.local"
_STATUS_OVERRIDE = os.getenv("ESP32_LIVE_STATUS_URL", "").strip()
_STREAM_OVERRIDE = os.getenv("ESP32_LIVE_STREAM_URL", "").strip()
# IP ma ESP32 vua lien lac (goi /health hoac do tim UDP). Uu tien hon esp32-01.local:
# tren Windows phan giai .local cham (hay qua timeout) hoac khong co.
_seen_ip = ""


def note_device_ip(ip: str) -> None:
    """Ghi nho IP cua ESP32 khi no goi toi may chu."""
    global _seen_ip
    if ip and ip != _seen_ip:
        log.info("ESP32 live: dung IP %s", ip)
        _seen_ip = ip


def _host() -> str:
    return _HOST_ENV or _seen_ip or HOST


def _status_url() -> str:
    return _STATUS_OVERRIDE or f"http://{_host()}/status"


def _stream_url() -> str:
    return _STREAM_OVERRIDE or f"http://{_host()}:81/stream"
MAX_VIEWERS = 3
_viewers = 0


def _check_key(request: Request, key: str) -> None:
    # <img> cannot send headers, so the stream also accepts ?key=.
    if not key:
        return
    given = request.headers.get("X-API-Key", "") or request.query_params.get("key", "")
    if not secrets.compare_digest(given, key):
        raise HTTPException(401, "Khóa dashboard chưa đúng.")


def attach_esp32_live(app, key: str) -> None:
    @app.get("/api/esp32/live/status")
    async def live_status(request: Request):
        _check_key(request, key)
        try:
            async with httpx.AsyncClient(timeout=2.5) as client:
                response = await client.get(_status_url())
                response.raise_for_status()
                data = response.json()
        except (httpx.HTTPError, ValueError) as exc:
            return JSONResponse(
                {"online": False, "host": _host(), "error": type(exc).__name__}, status_code=200
            )
        return {"online": True, "host": _host(), **data}

    @app.get("/api/esp32/live/stream")
    async def live_stream(request: Request):
        global _viewers
        _check_key(request, key)
        if _viewers >= MAX_VIEWERS:
            raise HTTPException(429, "Quá nhiều người đang xem live ESP32.")
        client = httpx.AsyncClient(timeout=httpx.Timeout(5.0, read=15.0))
        try:
            upstream = await client.send(client.build_request("GET", _stream_url()), stream=True)
            upstream.raise_for_status()
        except httpx.HTTPError as exc:
            await client.aclose()
            log.info("ESP32 live unavailable at %s: %s", _stream_url(), type(exc).__name__)
            raise HTTPException(503, f"Không kết nối được ESP32 ({_host()}).") from None

        async def relay():
            global _viewers
            _viewers += 1
            try:
                async for chunk in upstream.aiter_raw():
                    if await request.is_disconnected():
                        break
                    yield chunk
            except (httpx.HTTPError, asyncio.CancelledError):
                pass
            finally:
                _viewers -= 1
                await upstream.aclose()
                await client.aclose()

        return StreamingResponse(
            relay(),
            media_type=upstream.headers.get(
                "content-type", "multipart/x-mixed-replace;boundary=frame"
            ),
            headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"},
        )
