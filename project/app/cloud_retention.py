"""Delete terminal requests and their objects through the Storage API, never storage SQL."""

import os, time
from datetime import datetime, timezone, timedelta


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

    async def remove_job(self, job):
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
        await self.rt.call(
            self.rt.client.request,
            "DELETE",
            "/rest/v1/ai_requests?id=eq." + rid + "&status=in.(completed,acked,failed)",
        )

    async def run_once(self, force=False):
        if os.getenv("CLOUD_RETENTION_ENABLED", "1") != "1":
            return 0
        if not force and time.monotonic() - self.last < 300:
            return 0
        self.last = time.monotonic()
        now = datetime.now(timezone.utc) + timedelta(seconds=self.rt.client.clock_offset)
        days = max(1, int(os.getenv("CLOUD_RETENTION_DAYS", "7")))
        minutes = max(10, int(os.getenv("CLOUD_PROBE_RETENTION_MINUTES", "60")))
        old = (now - timedelta(days=days)).strftime("%Y-%m-%dT%H:%M:%SZ")
        probe = (now - timedelta(minutes=minutes)).strftime("%Y-%m-%dT%H:%M:%SZ")
        query = (
            "/rest/v1/ai_requests?select=id,owner_id,image_path,input,status,audio_path,screen_path&status=in.(completed,acked,failed)&or=(created_at.lt."
            + old
            + ",and(result->>confirmed.eq.false,created_at.lt."
            + probe
            + "))&order=created_at.desc&limit=100"
        )
        rows = await self.rt.call(self.rt.client.request, "GET", query)
        overflow = await self.rt.call(
            self.rt.client.request,
            "GET",
            "/rest/v1/ai_requests?select=id,owner_id,image_path,input,status,audio_path,screen_path"
            "&status=in.(completed,acked,failed)&order=created_at.asc&offset=50&limit=100",
        )
        known = {row["id"] for row in rows}
        rows.extend(row for row in overflow if row["id"] not in known)
        for job in rows:
            await self.remove_job(job)
        self.rt.state["retention_deleted"] = self.rt.state.get("retention_deleted", 0) + len(rows)
        return len(rows)
