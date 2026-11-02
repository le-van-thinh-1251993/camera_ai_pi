"""Dashboard commands and device reports; persist LAN changes until cloud reconnects."""

import json, re, time, uuid
from datetime import datetime, timezone
from fastapi import Depends, HTTPException
from fastapi.responses import Response
from pydantic import BaseModel, Field


class Command(BaseModel):
    action: str
    request_id: str = Field(min_length=1, max_length=100)
    count_all: bool = False


class DeviceControl:
    def __init__(self, runtime):
        self.rt = runtime
        self.epoch = uuid.uuid4().hex
        with self.rt.jobs.connect() as db:
            db.execute(
                "CREATE TABLE IF NOT EXISTS devices(id TEXT PRIMARY KEY,control TEXT NOT NULL DEFAULT '{}',state TEXT NOT NULL DEFAULT '{}',seen REAL NOT NULL DEFAULT 0,dirty INTEGER NOT NULL DEFAULT 0)"
            )

    def cloud_now(self):
        return (
            time.time() + getattr(self.rt.client, "clock_offset", 0)
            if self.rt.client
            else time.time()
        )

    def get(self, name):
        with self.rt.jobs.connect() as db:
            row = db.execute("SELECT * FROM devices WHERE id=?", (name,)).fetchone()
        if not row:
            return {"id": name, "control": {}, "state": {}, "seen": 0, "dirty": 0}
        data = dict(row)
        for key in ("control", "state"):
            data[key] = json.loads(data[key])
        return data

    def put(self, row):
        with self.rt.jobs.connect() as db:
            db.execute(
                "INSERT INTO devices(id,control,state,seen,dirty) VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET control=excluded.control,state=excluded.state,seen=excluded.seen,dirty=excluded.dirty",
                (
                    row["id"],
                    json.dumps(row["control"]),
                    json.dumps(row["state"]),
                    row["seen"],
                    row["dirty"],
                ),
            )

    def command(self, name, action, request_id, count_all=False):
        allowed = {"auto", "manual", "start", "stop", "capture", "home", "replay", "greeting"}
        if action not in allowed or not re.fullmatch(r"[A-Za-z0-9_-]{1,100}", request_id):
            raise ValueError("Lệnh không hợp lệ")
        row = self.get(name)
        if row["control"].get("id") == request_id:
            return
        now = self.cloud_now()
        row["control"] = {
            "id": request_id,
            "action": action,
            "issued": now,
            "expires": int(now + 60),
            "count_all": count_all,
        }
        self.put(row)

    def report(self, name, state):
        if not isinstance(state, dict) or len(json.dumps(state).encode()) > 4096:
            raise ValueError("Trạng thái quá lớn")
        row = self.get(name)
        row.update(state=state, seen=time.time(), dirty=1)
        self.put(row)

    async def sync(self):
        rows = await self.rt.call(
            self.rt.client.request,
            "GET",
            "/rest/v1/ai_devices?select=id,user_id,control,state,last_seen&enabled=eq.true",
        )
        self.rt.owners = {r["id"]: r["user_id"] for r in rows}
        for remote in rows:
            row = self.get(remote["id"])
            control = remote.get("control") or {}
            patch = {}
            if (
                row["control"].get("issued", 0) > control.get("issued", 0)
                and row["control"].get("expires", 0) > self.cloud_now()
            ):
                patch["control"] = row["control"]
            else:
                row["control"] = control
            seen = (
                datetime.fromisoformat(remote["last_seen"].replace("Z", "+00:00")).timestamp()
                - self.rt.client.clock_offset
                if remote.get("last_seen")
                else 0
            )
            if row["dirty"] and row["seen"] > seen:
                patch.update(
                    state=row["state"],
                    last_seen=datetime.fromtimestamp(
                        row["seen"] + self.rt.client.clock_offset, timezone.utc
                    ).isoformat(),
                )
            elif seen >= row["seen"]:
                row.update(state=remote.get("state") or {}, seen=seen)
            if patch:
                await self.rt.call(
                    self.rt.client.request,
                    "PATCH",
                    "/rest/v1/ai_devices?id=eq." + remote["id"],
                    patch,
                )
            newest = self.get(remote["id"])
            row["dirty"] = 0
            if newest["control"].get("issued", 0) > row["control"].get("issued", 0):
                row["control"] = newest["control"]
            if newest["seen"] > row["seen"]:
                row.update(state=newest["state"], seen=newest["seen"], dirty=newest["dirty"])
            self.put(row)

    def latest_row(self, name):
        with self.rt.jobs.connect() as db:
            row = db.execute(
                "SELECT id FROM jobs WHERE device=? AND result IS NOT NULL AND json_extract(result,'$.source')='esp32' ORDER BY updated DESC LIMIT 1",
                (name,),
            ).fetchone()
        return self.rt.jobs.get(row[0]) if row else None

    def snapshot(self, name):
        info = self.get(name)
        state = info["state"]
        control = info["control"]
        result = self.rt.jobs.get(state.get("result_id", ""))
        latest = self.latest_row(name)
        return {
            "device_id": name,
            "server_epoch": self.epoch,
            "mode": state.get("mode", "manual"),
            "running": bool(state.get("running", False)),
            "view": state.get("view", "home"),
            "online": time.time() - info["seen"] < 30,
            "revision": int(control.get("issued", 0) * 1000),
            "capture_pending": control.get("action") == "capture"
            and control.get("id") != state.get("control_id"),
            "hold_seconds": float(state.get("hold", 0)),
            "hold_required": 2,
            "frame_seq": int(latest["updated"] * 1000) if latest else -1,
            "can_capture": bool(state.get("has_frame")),
            "result": result["result"] if result else None,
            "audio": None,
            "audio_status": state.get("audio_status", "idle"),
            "preview_url": f"/api/cloud/device/{name}/preview.jpg",
            "control_pending": bool(
                control.get("id")
                and control.get("id") != state.get("control_id")
                and control.get("expires", 0) > self.cloud_now()
            ),
        }


def attach_cloud_control(app, auth):
    def controller(name):
        rt = getattr(app.state, "cloud_runtime", None)
        if not rt:
            raise HTTPException(503, "Đang khởi động")
        if name not in rt.owners:
            raise HTTPException(403, "Thiết bị chưa được đăng ký")
        return rt.controls

    @app.get("/api/cloud/device/{device}/state", dependencies=[Depends(auth)])
    async def state(device: str):
        return controller(device).snapshot(device)

    @app.post("/api/cloud/device/{device}/command", dependencies=[Depends(auth)])
    async def command(device: str, body: Command):
        c = controller(device)
        before = c.get(device)
        try:
            c.command(device, body.action, body.request_id, body.count_all)
            if not c.rt.client:
                raise RuntimeError("Cloud unavailable")
            await c.rt.call(
                c.rt.client.request,
                "PATCH",
                "/rest/v1/ai_devices?id=eq." + device,
                {"control": c.get(device)["control"]},
            )
        except ValueError as exc:
            raise HTTPException(422, str(exc))
        except Exception:
            c.put(before)
            raise HTTPException(503, "Không gửi được lệnh lên Supabase") from None
        c.rt.activity()
        return c.snapshot(device)

    @app.get("/api/cloud/device/{device}/preview.jpg", dependencies=[Depends(auth)])
    async def preview(device: str):
        c = controller(device)
        row = c.latest_row(device)
        if not row:
            raise HTTPException(404, "Chưa có ảnh gửi lên từ ESP32")
        image = c.rt.jobs.root / (row["id"] + ".preview.jpg")
        if not image.exists():
            image = c.rt.jobs.root / (row["id"] + ".jpg")
        return Response(
            image.read_bytes(), media_type="image/jpeg", headers={"Cache-Control": "no-store"}
        )
