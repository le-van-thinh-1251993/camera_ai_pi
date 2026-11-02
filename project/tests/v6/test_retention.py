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
