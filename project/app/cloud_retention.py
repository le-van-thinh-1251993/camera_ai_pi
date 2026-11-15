"""Keep Supabase within the Free plan (500 MB database, 1 GB Storage) while keeping as much as possible.

Results are the data used for statistics, so rows are kept far longer than images:
- CLOUD_KEEP_ROWS (default 80000, ~3.5 KB/row ~= 300-400 MB): beyond the newest N terminal
  requests the oldest rows are deleted together with their files.
- CLOUD_KEEP_IMAGES (default 24000, ~35 KB photo+screen ~= 850 MB): beyond the newest N the
  photo/screen objects are deleted but the row (gesture, count, time, device) is kept and
  marked input.images_pruned=true.
- CLOUD_RETENTION_DAYS (default 0 = never): optional age limit that deletes whole rows.
- Unconfirmed Auto probes (result.confirmed=false) are still removed after
  CLOUD_PROBE_RETENTION_MINUTES: they are not results.
Objects are always deleted through the Storage API, never storage SQL.
"""

import os, time
from datetime import datetime, timezone, timedelta

_COLS = "id,owner_id,image_path,input,status,audio_path,screen_path"
_TERMINAL = "status=in.(completed,acked,failed)"


def _env_int(name, default, low):
    try:
        return max(low, int(os.getenv(name, str(default))))
    except ValueError:
        return default


class CloudRetention:
    def __init__(self, runtime):
        self.rt = runtime
        self.last = 0
        self.last_orphans = 0

    async def object_prefix(self, bucket, prefix, depth=0):
        rows = await self.rt.call(
            self.rt.client.request,
            "POST",
            "/storage/v1/object/list/" + bucket,
            {"prefix": prefix, "limit": 100, "offset": 0},
        )
        files = []
        for row in rows:
            name = row.get("name", "")
            if name in ("", ".", "..") or "/" in name:
                continue
            if row.get("id"):
                files.append(prefix + name)
            elif depth < 3:
                files.extend(await self.object_prefix(bucket, prefix + name + "/", depth + 1))
        return files

    async def remove_job(self, job, keep_row=False):
        uid = job["owner_id"]
        rid = job["id"]
        prefix = f"{uid}/{rid}/"
        images = {job["image_path"]}
        images.update(
            f["path"]
            for f in (job.get("input") or {}).get("frames", [])
            if f["path"].startswith(prefix)
        )
        images.update(await self.object_prefix("ai-images", prefix))
        outputs = await self.object_prefix("ai-results", prefix)
        for name in ("screen_path", "audio_path"):
            if job.get(name) and job[name].startswith(prefix) and job[name] not in outputs:
                outputs.append(job[name])
        # Shared owner/voices/hash.wav is deliberately not under the request prefix.
        if images:
            await self.rt.call(
                self.rt.client.request,
                "DELETE",
                "/storage/v1/object/ai-images",
                {"prefixes": sorted(images)},
            )
        if outputs:
            await self.rt.call(
                self.rt.client.request,
                "DELETE",
                "/storage/v1/object/ai-results",
                {"prefixes": outputs},
            )
        if keep_row:
            # Giu hang ket qua de thong ke; danh dau da xoa anh de khong xu ly lai.
            await self.rt.call(
                self.rt.client.request,
                "PATCH",
                "/rest/v1/ai_requests?id=eq." + rid,
                {"input": {**(job.get("input") or {}), "images_pruned": True}, "screen_path": None},
            )
            return
        await self.rt.call(
            self.rt.client.request,
            "DELETE",
            "/rest/v1/ai_requests?id=eq." + rid + "&" + _TERMINAL,
        )

    async def run_once(self, force=False):
        if os.getenv("CLOUD_RETENTION_ENABLED", "1") != "1":
            return 0
        if not force and time.monotonic() - self.last < 300:
            return 0
        self.last = time.monotonic()
        now = datetime.now(timezone.utc) + timedelta(seconds=self.rt.client.clock_offset)
        get = lambda query: self.rt.call(self.rt.client.request, "GET", query)
        stamp = lambda t: t.strftime("%Y-%m-%dT%H:%M:%SZ")

        # 1) Mau Auto chua xac nhan (khong phai ket qua) + gioi han tuoi tuy chon -> xoa ca hang.
        minutes = _env_int("CLOUD_PROBE_RETENTION_MINUTES", 60, 10)
        cond = "and(result->>confirmed.eq.false,created_at.lt." + stamp(now - timedelta(minutes=minutes)) + ")"
        days = _env_int("CLOUD_RETENTION_DAYS", 0, 0)
        if days:
            cond += ",created_at.lt." + stamp(now - timedelta(days=days))
        rows = await get(
            f"/rest/v1/ai_requests?select={_COLS}&{_TERMINAL}&or=({cond})&order=created_at.desc&limit=100"
        )
        # 2) Vuot so hang toi da (gan 500 MB database) -> xoa hang cu nhat.
        keep_rows = _env_int("CLOUD_KEEP_ROWS", 80000, 100)
        known = {row["id"] for row in rows}
        rows.extend(
            row
            for row in await get(
                f"/rest/v1/ai_requests?select={_COLS}&{_TERMINAL}&order=created_at.desc&offset={keep_rows}&limit=100"
            )
            if row["id"] not in known
        )
        for job in rows:
            await self.remove_job(job)
        # 3) Vuot so anh toi da (gan 1 GB Storage) -> chi xoa anh, giu hang ket qua.
        keep_images = _env_int("CLOUD_KEEP_IMAGES", 24000, 100)
        pruned = 0
        for job in await get(
            f"/rest/v1/ai_requests?select={_COLS}&{_TERMINAL}&input->>images_pruned=is.null"
            f"&order=created_at.desc&offset={keep_images}&limit=100"
        ):
            if job["id"] not in known:
                await self.remove_job(job, keep_row=True)
                pruned += 1
        self.rt.state["retention_deleted"] = self.rt.state.get("retention_deleted", 0) + len(rows)
        self.rt.state["retention_images_pruned"] = self.rt.state.get("retention_images_pruned", 0) + pruned
        return len(rows) + pruned
