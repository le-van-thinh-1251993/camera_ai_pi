"""Supabase is the only inference input. Cache never bypasses a completed cloud request."""

import asyncio
import contextlib
import hashlib
import json
import logging
import os
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from app.supabase_client import SupabaseClient, CloudError
from app.local_jobs import LocalJobs
from app.cloud_protocol import validate_input, auto_decision, is_permanent
from app.cloud_retention import CloudRetention
from app.screen_renderer import screen_jpeg

log = logging.getLogger("ai-camera.cloud")


class CloudRuntime:
    def __init__(self, data, recognize, commit, voice):
        self.data = Path(data)
        self.jobs = LocalJobs(data)
        self.recognize = recognize
        self.commit = commit
        self.voice = voice
        self.client = None
        self.owners = {}
        self.completed = {}
        self.shared_voices = set()
        self.enabled = os.getenv("CLOUD_ENABLED", "1") == "1"
        self.local_fallback = os.getenv("CLOUD_LOCAL_FALLBACK", "1") == "1"
        self.state = {
            "enabled": self.enabled,
            "connected": False,
            "last_error": None,
            "processed": 0,
        }
        name = os.getenv("CLOUD_DEVICE_ID", "")
        owner = os.getenv("CLOUD_DEVICE_UID", "")
        if name and owner:
            self.owners[name] = str(uuid.UUID(owner))
        self.worker = os.getenv("CLOUD_WORKER_ID", "pi-ai-01")
        self.wake = asyncio.Event()
        self.control_wake = asyncio.Event()
        self.active_until = 0
        from app.cloud_control import DeviceControl

        self.controls = DeviceControl(self)
        self.retention = CloudRetention(self)

    async def call(self, method, *args, **kwargs):
        return await asyncio.to_thread(method, *args, **kwargs)

    def activity(self):
        self.active_until = time.monotonic() + 60
        self.wake.set()
        self.control_wake.set()

    def cache_completed(self, event):
        self.completed[event["id"]] = event
        if len(self.completed) > 200:
            self.completed.pop(next(iter(self.completed)))

    async def process_local_job(self, request_id, device, owner, images, options):
        """Process dashboard work locally while Supabase is unavailable."""
        rid = str(uuid.UUID(request_id))
        owner = str(uuid.UUID(owner))
        frames = validate_input(rid, owner, images[0][0], options)
        if len(frames) != len(images):
            raise ValueError("invalid_frame_count")
        await self.call(self.jobs.put_image, rid, device, owner, images[0][1], options)
        events = []
        for index, frame in enumerate(frames):
            raw = images[index][1]
            result = await self.recognize(
                raw,
                options.get("source", "browser"),
                focus=options.get("focus"),
                count_all=bool(options.get("count_all", False)),
                request_id=rid,
            )
            events.append(result)
        stable = (
            auto_decision(events, frames)
            if options.get("mode") == "auto" and len(frames) == 3
            else True
        )
        final = stable or int(options.get("retry_index", 0)) >= 4
        event = dict(events[-1])
        event.update(
            id=rid,
            pipeline_version=6,
            cloud=False,
            fallback=True,
            confirmed=final,
            stable=stable,
            frames_checked=len(frames),
            hold_ms=frames[-1]["ms"] - frames[0]["ms"],
            samples=[item["gesture"] for item in events],
        )
        if not stable:
            event.update(
                gesture="Unknown",
                finger_count=None,
                text="Chưa nhận diện được cử chỉ. Bạn vui lòng giữ tay rõ trong khung hình trong 2 giây.",
            )
        if final:
            event["audio_url"], event["tts"] = await self.voice(event["gesture"])
        else:
            event.update(audio_url=None, tts="not-confirmed")
        await self.call(self.jobs.complete, rid, event)
        await self.call(self.jobs.prune)
        self.cache_completed(event)
        if event["confirmed"]:
            await self.commit(event)
        return event

    async def renew(self, job):
        while True:
            await asyncio.sleep(25)
            if not await self.call(
                self.client.rpc, "ai_renew", {"p_id": job["id"], "p_token": job["lease_token"]}
            ):
                return

    async def assets(self, job, event):
        audio_path = None
        if event.get("audio_url"):
            name = Path(event["audio_url"]).name
            audio = self.data / "audio" / name
            if not audio.is_file() or len(name.split(".")[0]) != 64:
                raise ValueError("invalid_audio_cache")
            digest = hashlib.sha256(audio.read_bytes()).hexdigest()
            audio_path = f"{job['owner_id']}/voices/{digest}.wav"
            if audio_path not in self.shared_voices:
                try:
                    await self.call(self.client.download, "ai-results", audio_path, 2 * 1024 * 1024)
                except CloudError as exc:
                    if exc.status not in (400, 404):
                        raise
                    await self.call(
                        self.client.upload,
                        "ai-results",
                        audio_path,
                        audio.read_bytes(),
                        "audio/wav",
                    )
                self.shared_voices.add(audio_path)
        if (job.get("input") or {}).get("source") in ("browser", "upload"):
            return audio_path, None
        screen = await self.call(
            screen_jpeg,
            {"device_id": job["device_id"], "mode": "cloud", "view": "result", "result": event},
            480,
            320,
        )
        screen_path = f"{job['owner_id']}/{job['id']}/{job['lease_token']}/screen.jpg"
        await self.call(self.client.upload, "ai-results", screen_path, screen, "image/jpeg")
        return audio_path, screen_path

    async def process_job(self, job):
        rid = str(uuid.UUID(job["id"]))
        owner = str(uuid.UUID(job["owner_id"]))
        options = job.get("input") or {}
        frames = validate_input(rid, owner, job["image_path"], options)
        heartbeat = asyncio.create_task(self.renew(job))
        try:
            raw = await self.call(self.client.download, "ai-images", frames[0]["path"])
            row = await self.call(
                self.jobs.put_image, rid, job["device_id"], owner, raw, options
            )
            event = (
                row["result"]
                if row["result"] and row["result"].get("pipeline_version") == 6
                else None
            )
            if event is None:
                events = []
                for i, frame in enumerate(frames):
                    image = (
                        raw
                        if i == 0
                        else await self.call(self.client.download, "ai-images", frame["path"])
                    )
                    for attempt in range(10):
                        try:
                            if (
                                frame.get("sha256")
                                and hashlib.sha256(image).hexdigest() != frame["sha256"]
                            ):
                                raise ValueError("image_checksum_mismatch")
                            result = await self.recognize(
                                image,
                                options.get("source", "esp32"),
                                focus=options.get("focus"),
                                count_all=bool(options.get("count_all", False)),
                                request_id=rid,
                            )
                            break
                        except Exception as exc:
                            if getattr(exc, "status_code", None) != 429 or attempt == 9:
                                raise
                            await asyncio.sleep(0.3)
                    events.append(result)
                event = events[-1]
                stable = auto_decision(events, frames) if options.get("mode") == "auto" else True
                final = stable or int(options.get("retry_index", 0)) >= 4
                event.update(
                    id=rid,
                    pipeline_version=6,
                    cloud=True,
                    confirmed=final,
                    stable=stable,
                    frames_checked=len(frames),
                    hold_ms=frames[-1]["ms"] - frames[0]["ms"],
                    samples=[e["gesture"] for e in events],
                )
                if not stable:
                    event.update(
                        gesture="Unknown",
                        finger_count=None,
                        text="Chưa nhận diện được cử chỉ. Bạn vui lòng giữ tay rõ trong khung hình trong 2 giây.",
                    )
                if final:
                    event["audio_url"], event["tts"] = await self.voice(event["gesture"])
                else:
                    event.update(audio_url=None, tts="not-confirmed")
                if options.get("source") == "esp32" or not options.get("source"):
                    event.update(device_id=job["device_id"], speak_browser=False)
                else:
                    event.pop("device_id", None)
                await self.call(self.jobs.complete, rid, event)
                await self.call(self.jobs.prune)
            audio, screen = (None, None)
            if event["confirmed"]:
                audio, screen = await self.assets(job, event)
            public = {k: v for k, v in event.items() if k not in ("audio_url", "preview_url")}
            done = await self.call(
                self.client.rpc,
                "ai_complete",
                {
                    "p_id": rid,
                    "p_token": job["lease_token"],
                    "p_result": public,
                    "p_audio": audio,
                    "p_screen": screen,
                },
            )
            if done:
                self.cache_completed(event)
                if event["confirmed"]:
                    await self.commit(event)
                row = self.jobs.get(rid)
                self.jobs.mark_synced(rid, row["updated"])
                self.state["processed"] += 1
        finally:
            heartbeat.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await heartbeat

    async def run(self):
        if not self.enabled:
            return
        try:
            self.client = SupabaseClient(timeout=10)
        except ValueError:
            self.state["last_error"] = "missing_cloud_config"
            return
        # Probe the REST endpoint once before starting the worker loops.  This
        # makes /health reflect an actual Supabase connection instead of
        # reporting ready while the first claim call is still failing.
        try:
            await self.call(
                self.client.request,
                "GET",
                "/rest/v1/ai_devices?select=id&limit=1",
            )
            self.state.update(connected=True, last_error=None)
        except CloudError as exc:
            self.state.update(connected=False, last_error=exc.code)
        controls = asyncio.create_task(self.control_loop())
        maintenance = asyncio.create_task(self.maintenance_loop())
        try:
            await self.work_loop()
        finally:
            maintenance.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await maintenance
            controls.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await controls
            self.client.close()

    async def maintenance_loop(self):
        while True:
            try:
                await self.retention.run_once()
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                self.state["retention_error"] = type(exc).__name__
            await asyncio.sleep(300)

    async def control_loop(self):
        while True:
            self.control_wake.clear()
            try:
                await self.controls.sync()
                self.state.pop("control_error", None)
                if any(self.controls.get(name)["state"].get("running") for name in self.owners):
                    self.activity()
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                self.state["control_error"] = (
                    exc.code if isinstance(exc, CloudError) else type(exc).__name__
                )
            try:
                await asyncio.wait_for(
                    self.control_wake.wait(), 2 if time.monotonic() < self.active_until else 12
                )
            except asyncio.TimeoutError:
                pass

    async def work_loop(self):
        delay = 1.0
        pruned = 0
        while True:
            job = None
            self.wake.clear()
            try:
                await self.sync_local_outbox()
                rows = await self.call(
                    self.client.rpc, "ai_claim_v6", {"p_worker": self.worker, "p_seconds": 120}
                )
                self.state.update(connected=True, last_error=None)
                if rows:
                    job = rows[0]
                    self.activity()
                    await self.process_job(job)
                    delay = 0.4
                else:
                    delay = min(2 if time.monotonic() < self.active_until else 8, delay * 1.5)
                if time.monotonic() - pruned > 3600:
                    await self.call(self.jobs.prune)
                    pruned = time.monotonic()
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                code = (
                    exc.code
                    if isinstance(exc, CloudError)
                    else (
                        "image_" + str(exc.status_code)
                        if hasattr(exc, "status_code")
                        else type(exc).__name__
                    )
                )
                self.state.update(connected=not isinstance(exc, CloudError), last_error=code)
                log.warning("Cloud job error: %s", code)
                if job:
                    with contextlib.suppress(Exception):
                        await self.call(
                            self.client.rpc,
                            "ai_fail_v2",
                            {
                                "p_id": job["id"],
                                "p_token": job["lease_token"],
                                "p_error": code,
                                "p_permanent": is_permanent(exc),
                            },
                        )
                delay = min(8, max(1, delay * 1.5))
            try:
                await asyncio.wait_for(self.wake.wait(), timeout=delay)
            except asyncio.TimeoutError:
                pass

    async def sync_local_outbox(self):
        """Publish completed offline results after Supabase reconnects."""
        for row in await self.call(self.jobs.pending):
            result = row.get("result") or {}
            if not result.get("fallback") or not self.client:
                continue
            image = self.data / "hybrid" / (row["id"] + ".jpg")
            if not image.is_file():
                continue
            image_path = f"{row['owner']}/{row['id']}.jpg"
            await self.call(
                self.client.upload, "ai-images", image_path, image.read_bytes(), "image/jpeg"
            )
            payload = dict(row["input"])
            payload["pipeline_version"] = 6
            payload["source"] = payload.get("source", "browser")
            payload["frames"] = [
                {"path": image_path, "ms": 0, "sha256": hashlib.sha256(image.read_bytes()).hexdigest()}
            ]
            try:
                await self.call(
                    self.client.request,
                    "POST",
                    "/rest/v1/ai_requests",
                    {
                        "id": row["id"],
                        "device_id": row["device"],
                        "owner_id": row["owner"],
                        "image_path": image_path,
                        "input": payload,
                    },
                    headers={"Prefer": "return=minimal"},
                )
            except CloudError as exc:
                if exc.status != 409:
                    raise
            public = {key: value for key, value in result.items() if key != "audio_url"}
            await self.call(
                self.client.request,
                "PATCH",
                "/rest/v1/ai_requests?id=eq." + row["id"],
                {
                    "status": "completed",
                    "result": public,
                    "completed_at": datetime.now(timezone.utc).isoformat(),
                },
            )
            await self.call(self.jobs.mark_synced, row["id"], row["updated"])
            self.state["offline_synced"] = self.state.get("offline_synced", 0) + 1
