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
HOST = os.getenv("ESP32_LIVE_HOST", "esp32-01.local").strip()
STATUS_URL = os.getenv("ESP32_LIVE_STATUS_URL", f"http://{HOST}/status")
STREAM_URL = os.getenv("ESP32_LIVE_STREAM_URL", f"http://{HOST}:81/stream")
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
                response = await client.get(STATUS_URL)
                response.raise_for_status()
                data = response.json()
        except (httpx.HTTPError, ValueError) as exc:
            return JSONResponse(
                {"online": False, "host": HOST, "error": type(exc).__name__}, status_code=200
            )
        return {"online": True, "host": HOST, **data}

    @app.get("/api/esp32/live/stream")
    async def live_stream(request: Request):
        global _viewers
        _check_key(request, key)
        if _viewers >= MAX_VIEWERS:
            raise HTTPException(429, "Quá nhiều người đang xem live ESP32.")
        client = httpx.AsyncClient(timeout=httpx.Timeout(5.0, read=15.0))
        try:
            upstream = await client.send(client.build_request("GET", STREAM_URL), stream=True)
            upstream.raise_for_status()
        except httpx.HTTPError as exc:
            await client.aclose()
            log.info("ESP32 live unavailable at %s: %s", STREAM_URL, type(exc).__name__)
            raise HTTPException(503, f"Không kết nối được ESP32 ({HOST}).") from None

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
