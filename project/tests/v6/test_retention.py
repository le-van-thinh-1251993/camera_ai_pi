import unittest

from app.cloud_retention import CloudRetention


class Retention(unittest.IsolatedAsyncioTestCase):
    async def test_nested_screens_deleted_shared_voice_preserved(self):
        class Client:
            clock_offset = 0

            def __init__(self):
                self.deleted = {}

            def request(self, method, url, body=None):
                if method == "POST" and url.endswith("list/ai-results"):
                    if body["prefix"] == "owner/request/":
                        return [
                            {"name": "old-lease", "id": None},
                            {"name": "new-lease", "id": None},
                        ]
                    return [{"name": "screen.jpg", "id": "object-id"}]
                if method == "POST":
                    return []
                if method == "DELETE" and body:
                    self.deleted[url] = body["prefixes"]
                return []

        class Runtime:
            client = Client()

            async def call(self, function, *args):
                return function(*args)

        runtime = Runtime()
        await CloudRetention(runtime).remove_job(
            {
                "id": "request",
                "owner_id": "owner",
                "image_path": "owner/request.jpg",
                "screen_path": "owner/request/new-lease/screen.jpg",
                "audio_path": "owner/voices/shared.wav",
            }
        )
        self.assertEqual(
            set(runtime.client.deleted["/storage/v1/object/ai-results"]),
            {"owner/request/old-lease/screen.jpg", "owner/request/new-lease/screen.jpg"},
        )
        self.assertEqual(
            runtime.client.deleted["/storage/v1/object/ai-images"], ["owner/request.jpg"]
        )

    async def test_keeps_rows_prunes_only_old_images(self):
        old = {
            "id": "old",
            "owner_id": "owner",
            "image_path": "owner/old.jpg",
            "input": {"source": "esp32"},
            "status": "completed",
            "screen_path": "owner/old/screen.jpg",
            "audio_path": None,
        }

        class Client:
            clock_offset = 0

            def __init__(self):
                self.gets, self.patches, self.deletes = [], [], []

            def request(self, method, url, body=None):
                if method == "GET":
                    self.gets.append(url)
                    # Chi truy van "vuot so anh toi da" tra ve 1 hang cu.
                    return [dict(old)] if "images_pruned=is.null" in url else []
                if method == "PATCH":
                    self.patches.append((url, body))
                if method == "DELETE":
                    self.deletes.append(url)
                return []

        class Runtime:
            client = Client()
            state = {}

            async def call(self, function, *args):
                return function(*args)

        runtime = Runtime()
        count = await CloudRetention(runtime).run_once(force=True)
        client = runtime.client
        self.assertEqual(count, 1)
        self.assertFalse(any("offset=50&" in url for url in client.gets))  # het gioi han 50 hang
        self.assertTrue(any("offset=80000&" in url for url in client.gets))
        self.assertTrue(any("offset=24000&" in url for url in client.gets))
        self.assertFalse(any(url.startswith("/rest/v1/ai_requests") for url in client.deletes))
        self.assertIn("/storage/v1/object/ai-images", client.deletes)
        url, body = client.patches[0]
        self.assertEqual(url, "/rest/v1/ai_requests?id=eq.old")
        self.assertTrue(body["input"]["images_pruned"])
        self.assertEqual(body["input"]["source"], "esp32")
        self.assertIsNone(body["screen_path"])
