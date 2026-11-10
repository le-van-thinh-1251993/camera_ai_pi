"""Authenticated LAN endpoints for devices when Supabase is unreachable."""

import secrets
import uuid
from pathlib import Path

from fastapi import HTTPException, Request
from fastapi.responses import FileResponse


def attach_lan_web(app):
    def runtime():
        rt = getattr(app.state, "cloud_runtime", None)
        if not rt or not rt.local_fallback:
            raise HTTPException(503, "Nhận diện LAN chưa sẵn sàng")
        return rt

    def authorize(request: Request):
        expected = request.app.state.lan_api_key
        provided = request.headers.get("X-LAN-Key", "")
        if not expected:
            raise HTTPException(503, "Chưa cấu hình LAN_API_KEY trên Pi")
        if not secrets.compare_digest(provided, expected):
            raise HTTPException(401, "LAN key không đúng")

    def owner_for(rt, device):
        owner = rt.owners.get(device) or (device == rt.lan_device_id and rt.lan_owner_id)
        if not owner:
            raise HTTPException(403, "Thiết bị LAN chưa được đăng ký")
        try:
            return str(uuid.UUID(str(owner)))
        except ValueError:
            raise HTTPException(503, "CLOUD_DEVICE_UID không hợp lệ") from None

    @app.post("/api/lan/device/{device}/recognize")
    async def recognize(device: str, request: Request):
        authorize(request)
        rt = runtime()
        owner = owner_for(rt, device)
        raw = await request.body()
        if not (raw.startswith(b"\xff\xd8") or raw.startswith(b"\x89PNG\r\n\x1a\n")):
            raise HTTPException(415, "Chỉ nhận JPEG/PNG")
        try:
            rid = str(uuid.UUID(request.headers.get("X-Request-ID", str(uuid.uuid4()))))
            capture_mode = request.headers.get("X-Capture-Mode", "manual")
            options = {
                "pipeline_version": 6,
                "source": "esp32",
                "mode": "manual",
                "capture_mode": capture_mode,
                "frames": [{"path": f"{owner}/{rid}.jpg", "ms": 0}],
                "count_all": request.headers.get("X-Count-All", "").lower() == "true",
                "retry_index": max(0, min(4, int(request.headers.get("X-Retry-Index", "0")))),
            }
            event = await rt.process_local_job(
                rid, device, owner, [(options["frames"][0]["path"], raw, "image/jpeg")], options
            )
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from None
        result = dict(event)
        audio_url = result.get("audio_url")
        if audio_url:
            result["audio_url"] = f"/api/lan/device/{device}/jobs/{rid}/audio.wav"
        return {
            "id": rid,
            "status": "completed",
            "result": result,
            "cloud": False,
            "fallback": True,
        }

    @app.get("/api/lan/device/{device}/jobs/{request_id}/audio.wav")
    async def audio(device: str, request_id: str, request: Request):
        authorize(request)
        rt = runtime()
        owner_for(rt, device)
        try:
            rid = str(uuid.UUID(request_id))
        except ValueError:
            raise HTTPException(422, "Mã yêu cầu không hợp lệ") from None
        row = rt.jobs.get(rid)
        if not row or row.get("device") != device or not row.get("result"):
            raise HTTPException(404, "Không có âm thanh")
        url = row["result"].get("audio_url", "")
        name = Path(url).name
        if (
            len(name) != 68
            or not name.endswith(".wav")
            or any(c not in "0123456789abcdef" for c in name[:-4])
        ):
            raise HTTPException(404, "Không có âm thanh")
        path = rt.data / "audio" / name
        if not path.is_file():
            raise HTTPException(404, "Không có âm thanh")
        return FileResponse(path, media_type="audio/wav")

    @app.get("/api/lan/device/{device}/jobs/{request_id}/screen.jpg")
    async def screen(device: str, request_id: str, request: Request):
        authorize(request)
        rt = runtime()
        owner_for(rt, device)
        try:
            rid = str(uuid.UUID(request_id))
        except ValueError:
            raise HTTPException(422, "Mã yêu cầu không hợp lệ") from None
        row = rt.jobs.get(rid)
        path = rt.jobs.root / (rid + ".screen.jpg")
        if not row or row.get("device") != device or not row.get("result") or not path.is_file():
            raise HTTPException(404, "Không có ảnh màn hình")
        return FileResponse(path, media_type="image/jpeg")

    @app.post("/api/lan/device/{device}/exchange")
    async def exchange(device: str, request: Request):
        authorize(request)
        rt = runtime()
        owner_for(rt, device)
        try:
            state = await request.json()
        except ValueError:
            raise HTTPException(400, "Trạng thái không hợp lệ") from None
        if not isinstance(state, dict):
            raise HTTPException(400, "Trạng thái không hợp lệ")
        rt.controls.report(device, state)
        snapshot = rt.controls.snapshot(device)
        if not snapshot["control_pending"]:
            snapshot["control"] = {}
        return snapshot

    @app.post("/api/lan/device/{device}/jobs/{request_id}/ack")
    async def ack(device: str, request_id: str, request: Request):
        authorize(request)
        rt = runtime()
        owner_for(rt, device)
        try:
            rid = str(uuid.UUID(request_id))
            body = await request.json()
        except (ValueError, TypeError):
            raise HTTPException(400, "ACK không hợp lệ") from None
        row = rt.jobs.get(rid)
        if not row or row.get("device") != device:
            raise HTTPException(404, "Không có yêu cầu")
        rt.jobs.ack(rid, body.get("displayed") is True, body.get("played") is True)
        return {"ok": True}
