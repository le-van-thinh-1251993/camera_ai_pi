"""Submit browser images to Storage and read only results committed to Supabase."""

import asyncio, hashlib, json, os, time, uuid, logging
from fastapi import Depends, HTTPException, Request
from fastapi.responses import FileResponse, Response
from starlette.datastructures import UploadFile
from app.cloud_protocol import validate_input
from app.supabase_client import CloudError


def attach_cloud_web(app, auth):
    slots = asyncio.Semaphore(2)
    remote_cache = {}

    def runtime():
        rt = getattr(app.state, "cloud_runtime", None)
        if not rt or not rt.enabled:
            raise HTTPException(
                503, "Cloud worker chưa sẵn sàng. Xem /api/diagnostics sau khi đăng nhập."
            )
        return rt

    async def job_row(rt, rid):
        if not rt.client:
            raise HTTPException(503, "Supabase đang mất kết nối; chưa có bản ghi cục bộ cho yêu cầu này.")
        try:
            rid = str(uuid.UUID(rid))
        except ValueError:
            raise HTTPException(422, "Mã yêu cầu không hợp lệ")
        cached = remote_cache.get(rid)
        if cached and time.monotonic() - cached[0] < 1:
            return cached[1]
        try:
            row = await rt.call(rt.client.get_job, rid)
        except CloudError:
            raise HTTPException(
                503, "Không kết nối được Supabase; không dùng nhận diện cục bộ thay thế."
            ) from None
        if not row or row["owner_id"] != rt.owners.get(row["device_id"]):
            raise HTTPException(404, "Không có yêu cầu")
        if len(remote_cache) > 200:
            remote_cache.pop(next(iter(remote_cache)))
        remote_cache[rid] = (time.monotonic(), row)
        return row

    async def submit(request: Request, source):
        rt = runtime()
        name = os.getenv("CLOUD_DEVICE_ID", "esp32-01")
        owner = rt.owners.get(name) or os.getenv("CLOUD_DEVICE_UID", "").strip()
        if not owner:
            raise HTTPException(503, "Thiết bị Cloud chưa được đăng ký")
        if not request.headers.get("content-type", "").startswith("multipart/form-data"):
            raise HTTPException(415, "Cần giao diện Cloud v6. Nhấn Ctrl+F5 rồi gửi lại.")
        form = await request.form(max_files=3, max_fields=12, max_part_size=15 * 1024 * 1024)
        uploads = [
            item
            for key, item in form.multi_items()
            if key in ("file", "files") and isinstance(item, UploadFile)
        ]
        try:
            rid = str(uuid.UUID(str(form.get("request_id") or uuid.uuid4())))
            mode = str(form.get("mode", "upload" if source == "upload" else "manual"))
            capture_mode = str(form.get("capture_mode", mode))
            single_auto = capture_mode == "auto"
            expected = 1 if single_auto else (3 if mode == "auto" else 1)
            if len(uploads) != expected:
                raise HTTPException(422, "Bản single chỉ nhận 1 ảnh cho mỗi lượt")
            times = json.loads(str(form.get("captures_ms", "[0]")))
            if len(times) != expected:
                raise HTTPException(422, "Thiếu thời điểm chụp")
            focus = json.loads(str(form.get("focus", "null")))
            if focus is None and form.get("focus_x") is not None:
                focus = [float(form["focus_x"]), float(form["focus_y"])]
            options = {
                "pipeline_version": 6,
                "source": source,
                "mode": "manual" if single_auto else mode,
                "capture_mode": capture_mode,
                "frames": [],
                "focus": focus,
                "count_all": str(form.get("count_all", "false")).lower() == "true",
                "retry_index": max(0, min(4, int(form.get("retry_index", "0")))),
            }
            images = []
            limit = (
                (int(os.getenv("MAX_PHOTO_MB", "15")) if source == "upload" else 3) * 1024 * 1024
            )
            for i, file in enumerate(uploads):
                raw = await file.read(limit + 1)
                if len(raw) > limit:
                    raise HTTPException(413, "Ảnh vượt giới hạn dung lượng")
                if not (raw.startswith(b"\xff\xd8") or raw.startswith(b"\x89PNG\r\n\x1a\n")):
                    raise HTTPException(415, "Chỉ nhận ảnh JPEG/PNG")
                path = f"{owner}/{rid}.jpg" if i == 0 else f"{owner}/{rid}/{i}.jpg"
                options["frames"].append(
                    {"path": path, "ms": times[i], "sha256": hashlib.sha256(raw).hexdigest()}
                )
                images.append(
                    (path, raw, "image/png" if raw.startswith(b"\x89PNG") else "image/jpeg")
                )
            validate_input(rid, owner, images[0][0], options)
            if (
                rt.local_fallback
                and (not rt.client or not rt.state["connected"])
            ):
                event = await rt.process_local_job(rid, name, owner, images, options)
                return {
                    "id": rid,
                    "status": "completed",
                    "status_url": "/api/jobs/" + rid,
                    "result": event,
                    "cloud": False,
                    "fallback": True,
                }
            if not rt.client or not rt.state["connected"]:
                raise HTTPException(
                    503,
                    "Supabase đang mất kết nối; nhận diện cục bộ đang tắt.",
                )
            async with slots:
                prior = await rt.call(rt.client.get_job, rid)
                if prior:
                    if prior["owner_id"] != owner or prior.get("input") != options:
                        raise HTTPException(409, "Mã yêu cầu đã được dùng cho ảnh khác")
                    return {
                        "id": rid,
                        "status": prior["status"],
                        "status_url": "/api/jobs/" + rid,
                        "cloud": True,
                    }
                pending = await rt.call(
                    rt.client.request,
                    "GET",
                    "/rest/v1/ai_requests?status=in.(pending,processing)&select=id&limit=20",
                )
                if len(pending) >= 20:
                    raise HTTPException(429, "Hàng đợi Cloud đang đầy. Thử lại sau.")
                for path, raw, mime in images:
                    await rt.call(rt.client.upload, "ai-images", path, raw, mime)
                await rt.call(
                    rt.client.request,
                    "POST",
                    "/rest/v1/ai_requests",
                    {
                        "id": rid,
                        "device_id": name,
                        "owner_id": owner,
                        "image_path": images[0][0],
                        "input": options,
                    },
                    headers={"Prefer": "return=minimal"},
                )
            rt.activity()
            return {"id": rid, "status": "pending", "status_url": "/api/jobs/" + rid, "cloud": True}
        except CloudError as exc:
            logging.getLogger("ai-camera.cloud").warning(
                "Submit failed: HTTP %s code=%s", exc.status, exc.code
            )
            transient = exc.status in (0, 502, 503, 504) or exc.code.startswith("network_")
            if rt.local_fallback and transient and images:
                rt.state.update(connected=False, last_error=exc.code)
                try:
                    event = await rt.process_local_job(rid, name, owner, images, options)
                except Exception:
                    logging.getLogger("ai-camera.cloud").exception(
                        "Local fallback failed after Cloud submit failure"
                    )
                else:
                    return {
                        "id": rid,
                        "status": "completed",
                        "status_url": "/api/jobs/" + rid,
                        "result": event,
                        "cloud": False,
                        "fallback": True,
                    }
            if exc.status == 404 or exc.code in ("PGRST204", "PGRST205"):
                raise HTTPException(503, "Cần chạy SQL 003_cloud_only.sql trong Supabase") from None
            raise HTTPException(
                503, "Không gửi được ảnh lên Supabase. Chưa có kết quả nhận diện."
            ) from None
        except (ValueError, TypeError, KeyError):
            raise HTTPException(422, "Thông tin ảnh hoặc thời điểm chụp không hợp lệ") from None
        finally:
            for file in uploads:
                await file.close()

    @app.post("/api/upload", status_code=202, dependencies=[Depends(auth)])
    async def upload(request: Request):
        return await submit(request, "upload")

    @app.post("/api/camera", status_code=202, dependencies=[Depends(auth)])
    async def camera(request: Request):
        return await submit(request, "browser")

    @app.get("/api/jobs/{request_id}", dependencies=[Depends(auth)])
    async def result(request_id: str):
        rt = runtime()
        try:
            rid = str(uuid.UUID(request_id))
        except ValueError:
            raise HTTPException(422, "Mã yêu cầu không hợp lệ")
        cached = rt.completed.get(rid)
        if cached:
            event = dict(cached)
            event["preview_url"] = f"/api/jobs/{rid}/preview.jpg"
            return {
                "id": rid,
                "status": "completed",
                "result": event,
                "cloud": bool(event.get("cloud")),
                "fallback": bool(event.get("fallback")),
            }
        local = rt.jobs.get(rid)
        if local and local.get("result"):
            event = dict(local["result"])
            event["preview_url"] = f"/api/jobs/{rid}/preview.jpg"
            return {
                "id": rid,
                "status": "completed",
                "result": event,
                "cloud": False,
                "fallback": True,
            }
        row = await job_row(rt, rid)
        if row["status"] not in ("completed", "acked"):
            return {
                "id": rid,
                "status": row["status"],
                "error_code": row.get("error_code"),
                "cloud": True,
            }
        event = dict(row["result"])
        event["preview_url"] = f"/api/jobs/{rid}/preview.jpg"
        event["audio_url"] = f"/api/jobs/{rid}/audio.wav" if row.get("audio_path") else None
        return {"id": rid, "status": row["status"], "result": event, "cloud": True}

    @app.get("/api/jobs/{request_id}/{asset}", dependencies=[Depends(auth)])
    async def asset(request_id: str, asset: str):
        rt = runtime()
        local = rt.jobs.get(request_id)
        if local and local.get("result"):
            if asset == "preview.jpg":
                path = rt.jobs.root / (local["id"] + ".preview.jpg")
                if path.is_file():
                    return FileResponse(path, media_type="image/jpeg")
            raise HTTPException(404, "Không có file")
        row = await job_row(rt, request_id)
        rid = row["id"]
        if asset == "preview.jpg":
            local = rt.jobs.root / (rid + ".preview.jpg")
            if local.is_file():
                return FileResponse(
                    local, media_type="image/jpeg", headers={"Cache-Control": "no-store"}
                )
            data = await rt.call(
                rt.client.download,
                "ai-images",
                (row.get("input", {}).get("frames") or [{"path": row["image_path"]}])[-1]["path"],
            )
            return Response(
                data,
                media_type="image/png" if data.startswith(b"\x89PNG") else "image/jpeg",
                headers={"Cache-Control": "no-store"},
            )
        if asset == "audio.wav" and row.get("audio_path"):
            data = await rt.call(
                rt.client.download, "ai-results", row["audio_path"], 2 * 1024 * 1024
            )
            return Response(data, media_type="audio/wav")
        raise HTTPException(404, "Không có file")

    @app.post("/api/jobs/{request_id}/ack", dependencies=[Depends(auth)])
    async def ack(request_id: str, request: Request):
        rt = runtime()
        local = rt.jobs.get(request_id)
        if local and local.get("result"):
            body = await request.json()
            rt.jobs.ack(request_id, body.get("displayed") is True, body.get("played") is True)
            return {"ok": True, "fallback": True}
        row = await job_row(rt, request_id)
        body = await request.json()
        if row["status"] not in ("completed", "acked") or not row.get("result", {}).get(
            "confirmed", True
        ):
            raise HTTPException(409, "Chưa có kết quả cuối")
        from datetime import datetime, timezone

        update = {"status": "acked", "acked_at": datetime.now(timezone.utc).isoformat()}
        if body.get("displayed") is True:
            update["ack_displayed"] = True
        if body.get("played") is True:
            update["ack_played"] = True
        await rt.call(rt.client.request, "PATCH", "/rest/v1/ai_requests?id=eq." + row["id"], update)
        remote_cache.pop(row["id"], None)
        return {"ok": True}
