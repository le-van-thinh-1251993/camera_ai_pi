import unittest, tempfile
from app.local_jobs import LocalJobs


class Cache(unittest.TestCase):
    def test_idempotent_result_and_conflicting_image(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = LocalJobs(directory)
            rid = "00000000-0000-4000-8000-000000000003"
            cache.put_image(rid, "esp32-01", "owner", b"first")
            cache.complete(rid, {"id": rid, "cloud": True})
            self.assertTrue(
                LocalJobs(directory).put_image(rid, "esp32-01", "owner", b"first")["result"][
                    "cloud"
                ]
            )
            with self.assertRaises(ValueError):
                cache.put_image(rid, "esp32-01", "owner", b"second")


if __name__ == "__main__":
    unittest.main()
