"""Core nối với module AI: dùng câu AI khi được, luôn lùi về câu cố định khi AI lỗi/tắt."""

import base64
import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import httpx

from app.ai_client import AiClient
from app.cloud_worker import CloudRuntime
from app.stats import compute_stats

WAV = b"RIFF" + b"\x24\x00\x00\x00" + b"WAVEfmt " + b"\x00" * 32
RID = "00000000-0000-4000-8000-000000000010"
OWNER = "00000000-0000-4000-8000-000000000011"


def module_reply(request):
    body = json.loads(request.content)
    assert body["finger_count"] == 3 and body["image_b64"]
    return httpx.Response(
        200,
        json={
            "text": "Ba ngón tay, ba điều may",
            "audio_wav_b64": base64.b64encode(WAV).decode(),
            "model": "gemini-test",
            "latency_ms": 1234,
            "image_sent": True,
        },
    )


class AiClientTest(unittest.IsolatedAsyncioTestCase):
    async def test_saves_wav_by_hash_for_existing_audio_routes(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.dict(os.environ, {"AI_MODULE_URL": "http://ai.local"}):
                client = AiClient(directory, httpx.MockTransport(module_reply))
            result = await client.meaning(RID, 3, "esp32", b"jpeg")
            name = hashlib.sha256(WAV).hexdigest() + ".wav"
            self.assertEqual(result["audio_url"], "/api/audio/" + name)
            self.assertEqual((Path(directory) / "audio" / name).read_bytes(), WAV)
            self.assertEqual(result["meta"]["status"], "ok")
            self.assertEqual(result["meta"]["model"], "gemini-test")

    async def test_failures_return_none_with_reason(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.dict(os.environ, {"AI_MODULE_URL": ""}):
                self.assertIsNone(await AiClient(directory).meaning(RID, 3, "", b""))
            cases = [
                (lambda r: httpx.Response(429, json={"detail": "quota"}), "quota"),
                (
                    lambda r: httpx.Response(200, json={"text": "x", "audio_wav_b64": "AAAA"}),
                    "ValueError",
                ),
                (lambda r: (_ for _ in ()).throw(httpx.ConnectError("down")), "ConnectError"),
            ]
            for handler, reason in cases:
                with mock.patch.dict(os.environ, {"AI_MODULE_URL": "http://ai.local"}):
                    client = AiClient(directory, httpx.MockTransport(handler))
                self.assertIsNone(await client.meaning(RID, 3, "", b"x"))
                self.assertEqual(client.last_error, reason)


class RuntimeSpeak(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.voiced = []

        async def voice(key):
            self.voiced.append(key)
            return "/api/audio/" + "a" * 64 + ".wav", "recording"

        self.runtime = CloudRuntime(self.tmp.name, None, None, voice)

    async def asyncTearDown(self):
        self.tmp.cleanup()

    def fake_ai(self, result, error=None):
        calls = []

        async def meaning(rid, count, source, image):
            calls.append((rid, count, source, image))
            self.runtime.ai.last_error = error
            return result

        self.runtime.ai.meaning = meaning
        return calls

    async def test_ai_text_and_voice_replace_fixed_phrase(self):
        calls = self.fake_ai(
            {
                "text": "Câu AI",
                "audio_url": "/api/audio/" + "b" * 64 + ".wav",
                "meta": {"status": "ok"},
            }
        )
        (self.runtime.jobs.root / (RID + ".preview.jpg")).write_bytes(b"preview")
        event = {"id": RID, "gesture": "Count_3", "finger_count": 3, "text": "Câu cố định"}
        await self.runtime.speak(event, {"ai": True, "source": "esp32"}, b"raw")
        self.assertEqual(event["text"], "Câu AI")
        self.assertEqual(event["tts"], "ai")
        self.assertEqual(event["ai"]["status"], "ok")
        self.assertEqual(calls[0][3], b"preview")  # gửi ảnh đã thu nhỏ của MediaPipe
        self.assertEqual(self.voiced, [])

    async def test_fallback_keeps_fixed_phrase_and_recording(self):
        self.fake_ai(None, "timeout")
        event = {"id": RID, "gesture": "Count_3", "finger_count": 3, "text": "Câu cố định"}
        await self.runtime.speak(event, {"ai": True}, b"raw")
        self.assertEqual(event["text"], "Câu cố định")
        self.assertEqual(event["tts"], "recording")
        self.assertEqual(event["ai"], {"status": "fallback", "error": "timeout"})

    async def test_ai_not_called_without_count_or_when_off(self):
        calls = self.fake_ai({"text": "x"})
        unknown = {"id": RID, "gesture": "Unknown", "finger_count": None, "text": "Chưa rõ"}
        await self.runtime.speak(unknown, {"ai": True}, b"raw")
        self.assertEqual(unknown["ai"]["status"], "skipped")
        normal = {"id": RID, "gesture": "Count_2", "finger_count": 2, "text": "Hai"}
        await self.runtime.speak(normal, {}, b"raw")
        self.assertNotIn("ai", normal)
        self.assertEqual(calls, [])
        self.assertEqual(self.voiced, ["Unknown", "Count_2"])

    async def test_local_job_end_to_end(self):
        async def recognize(raw, source, focus=None, count_all=False, request_id=None):
            return {"gesture": "Count_3", "finger_count": 3, "text": "Câu cố định"}

        async def commit(event):
            pass

        self.runtime.recognize = recognize
        self.runtime.commit = commit
        self.fake_ai(
            {
                "text": "Câu AI",
                "audio_url": "/api/audio/" + "c" * 64 + ".wav",
                "meta": {"status": "ok"},
            }
        )
        path = f"{OWNER}/{RID}.jpg"
        event = await self.runtime.process_local_job(
            RID,
            "esp32-01",
            OWNER,
            [(path, b"\xff\xd8jpeg", "image/jpeg")],
            {
                "mode": "manual",
                "capture_mode": "auto",
                "frames": [{"path": path, "ms": 0}],
                "ai": True,
            },
        )
        self.assertEqual(event["text"], "Câu AI")
        self.assertEqual(self.runtime.jobs.get(RID)["result"]["tts"], "ai")


class AiStats(unittest.TestCase):
    def test_counts_ai_usage(self):
        events = [
            {"time": "2026-11-16T08:00:00+07:00", "finger_count": 3, "ai": {"status": "ok"}},
            {"time": "2026-11-16T08:01:00+07:00", "finger_count": 2, "ai": {"status": "fallback"}},
            {"time": "2026-11-16T08:02:00+07:00", "finger_count": 1},
        ]
        self.assertEqual(compute_stats(events, "all")["ai"], {"ok": 1, "fallback": 1})


if __name__ == "__main__":
    unittest.main()
