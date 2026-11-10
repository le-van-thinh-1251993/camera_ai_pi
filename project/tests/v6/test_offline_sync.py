import tempfile
import unittest

from app.cloud_worker import CloudRuntime


class OfflineSync(unittest.IsolatedAsyncioTestCase):
    async def test_completed_fallback_is_published_once(self):
        requests = []

        class Client:
            def upload(self, *args):
                requests.append(("upload", args))

            def request(self, *args, **kwargs):
                requests.append(("request", args, kwargs))

        async def recognize(*args, **kwargs):
            return {"gesture": "Count_2", "finger_count": 2}

        async def commit(event):
            return None

        async def voice(key):
            return None, "recording"

        with tempfile.TemporaryDirectory() as directory:
            runtime = CloudRuntime(directory, recognize, commit, voice)
            runtime.client = Client()
            rid = "00000000-0000-4000-8000-000000000006"
            owner = "00000000-0000-4000-8000-000000000007"
            runtime.jobs.put_image(
                rid,
                "browser",
                owner,
                b"\xff\xd8offline",
                {"source": "browser", "mode": "manual"},
            )
            runtime.jobs.complete(
                rid,
                {
                    "id": rid,
                    "pipeline_version": 6,
                    "fallback": True,
                    "confirmed": True,
                    "gesture": "Count_2",
                },
            )
            await runtime.sync_local_outbox()
            self.assertEqual(len(requests), 3)
            self.assertEqual(runtime.jobs.get(rid)["synced"], 1)


if __name__ == "__main__":
    unittest.main()
