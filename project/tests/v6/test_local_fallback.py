import tempfile
import unittest
from pathlib import Path

from app.cloud_worker import CloudRuntime


class LocalFallback(unittest.IsolatedAsyncioTestCase):
    async def test_processes_and_persists_when_cloud_is_unavailable(self):
        events = []

        async def recognize(raw, source, focus=None, count_all=False, request_id=None):
            Path(data, "hybrid", request_id + ".preview.jpg").write_bytes(b"jpeg")
            return {"gesture": "Count_2", "finger_count": 2, "text": "Hai ngón"}

        async def commit(event):
            events.append(event)

        async def voice(key):
            return "/api/audio/" + ("a" * 64) + ".wav", "recording"

        with tempfile.TemporaryDirectory() as directory:
            data = directory
            runtime = CloudRuntime(directory, recognize, commit, voice)
            rid = "00000000-0000-4000-8000-000000000004"
            owner = "00000000-0000-4000-8000-000000000005"
            path = f"{owner}/{rid}.jpg"
            result = await runtime.process_local_job(
                rid,
                "browser",
                owner,
                [(path, b"\xff\xd8image", "image/jpeg")],
                {
                    "pipeline_version": 6,
                    "source": "browser",
                    "mode": "manual",
                    "capture_mode": "manual",
                    "frames": [{"path": path, "ms": 0}],
                    "focus": None,
                    "count_all": False,
                    "retry_index": 0,
                },
            )
            self.assertTrue(result["fallback"])
            self.assertEqual(runtime.jobs.get(rid)["result"]["gesture"], "Count_2")
            self.assertEqual(len(events), 1)


if __name__ == "__main__":
    unittest.main()
