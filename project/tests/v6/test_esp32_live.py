import os
import unittest

# Port 9 (discard) is closed on test machines: the ESP32 is "offline".
os.environ["ESP32_LIVE_STATUS_URL"] = "http://127.0.0.1:9/status"
os.environ["ESP32_LIVE_STREAM_URL"] = "http://127.0.0.1:9/stream"

from fastapi import FastAPI  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

from app.esp32_live import attach_esp32_live  # noqa: E402


def client(key=""):
    app = FastAPI()
    attach_esp32_live(app, key)
    return TestClient(app)


class Esp32Live(unittest.TestCase):
    def test_offline_status_is_reported_not_raised(self):
        response = client().get("/api/esp32/live/status")
        self.assertEqual(response.status_code, 200)
        self.assertFalse(response.json()["online"])

    def test_stream_unavailable_returns_503(self):
        self.assertEqual(client().get("/api/esp32/live/stream").status_code, 503)

    def test_key_required_via_header_or_query(self):
        protected = client("secret")
        self.assertEqual(protected.get("/api/esp32/live/status").status_code, 401)
        self.assertEqual(protected.get("/api/esp32/live/status?key=secret").status_code, 200)
        self.assertEqual(
            protected.get("/api/esp32/live/status", headers={"X-API-Key": "secret"}).status_code,
            200,
        )


if __name__ == "__main__":
    unittest.main()
