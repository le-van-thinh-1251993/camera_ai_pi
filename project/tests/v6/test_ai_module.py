"""Module AI độc lập: làm sạch câu, gọi Gemini (giả lập HTTP), API /v1/meaning, hạn mức."""

import asyncio
import base64
import io
import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

import httpx
from fastapi.testclient import TestClient
from PIL import Image

from ai_module import config, gemini, quota, tts
from ai_module.server import create_app
from ai_module.text import clean

WAV = b"RIFF" + b"\x24\x00\x00\x00" + b"WAVEfmt " + b"\x00" * 32


def jpeg(size=(1600, 1200)):
    out = io.BytesIO()
    Image.new("RGB", size, (40, 120, 200)).save(out, format="JPEG")
    return out.getvalue()


def make_cfg(directory, **changes):
    cfg = config.Config(
        api_key="test-key",
        provider="gemini",
        replay_fallback=False,
        replay_delay=0,
        model="gemini-2.5-flash",
        fallback_model="gemini-lite-test",
        thinking_budget="0",
        thinking_level="low",
        style="vui",
        max_chars=100,
        daily_limit=50,
        send_image=True,
        image_side=512,
        gemini_timeout=5,
        fallback_timeout=3,
        voice="vi-VN-HoaiMyNeural",
        voice_rate="+0%",
        tts_order=("gemini", "edge"),
        tts_models=("tts-a", "tts-b"),
        tts_voice="Kore",
        tts_style="Giọng nữ Hà Nội, nói nhanh, liền mạch",
        tts_timeout=5,
        data_dir=Path(directory),
        host="127.0.0.1",
        port=8010,
    )
    return replace(cfg, **changes)


def gemini_reply(text):
    return {"candidates": [{"content": {"parts": [{"text": json.dumps({"y_nghia": text})}]}}]}


class CleanText(unittest.TestCase):
    def test_removes_emoji_and_keeps_vietnamese(self):
        self.assertEqual(clean("Ba ngón 🌟 – điều “may mắn”!\n", 100), 'Ba ngón – điều "may mắn"!')

    def test_cuts_at_word_boundary(self):
        out = clean("một hai ba bốn năm sáu bảy tám chín mười", 20)
        self.assertLessEqual(len(out), 20)
        self.assertTrue(out.endswith("…"))
        self.assertNotIn("  ", out)

    def test_drops_non_latin_script(self):
        self.assertEqual(clean("Xin chào 你好 bạn", 100), "Xin chào bạn")


class GeminiProtocol(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        quota.clear()
        self.addCleanup(quota.clear)
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = make_cfg(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_request_has_image_schema_and_rules(self):
        body = gemini.build_request(self.cfg, 3, b"jpg")
        parts = body["contents"][0]["parts"]
        self.assertEqual(parts[0]["inlineData"]["mimeType"], "image/jpeg")
        self.assertIn("3 ngón", parts[1]["text"])
        self.assertEqual(body["generationConfig"]["responseMimeType"], "application/json")
        self.assertEqual(body["generationConfig"]["thinkingConfig"], {"thinkingBudget": 0})
        self.assertIn("không nhận xét danh tính", body["systemInstruction"]["parts"][0]["text"])
        no_image = gemini.build_request(replace(self.cfg, thinking_budget=""), 3, None)
        self.assertEqual(len(no_image["contents"][0]["parts"]), 1)
        self.assertNotIn("thinkingConfig", no_image["generationConfig"])
        # model doi moi: khong gui thinkingBudget (tranh loi 400 lam hong ca duong du phong)
        lite = gemini.build_request(self.cfg, 3, None, "gemini-3.5-flash-lite")
        self.assertNotIn("thinkingConfig", lite["generationConfig"])

    def test_parse_variants(self):
        self.assertEqual(gemini.parse_response(gemini_reply("Ba điều may")), "Ba điều may")
        fenced = {
            "candidates": [{"content": {"parts": [{"text": '```json\n{"y_nghia": "A"}\n```'}]}}]
        }
        self.assertEqual(gemini.parse_response(fenced), "A")
        for bad, code in [
            ({"promptFeedback": {"blockReason": "SAFETY"}}, "blocked"),
            ({"candidates": [{"finishReason": "SAFETY"}]}, "blocked"),
            (
                {"candidates": [{"content": {"parts": [{"text": "không phải json"}]}}]},
                "bad_response",
            ),
        ]:
            with self.assertRaises(gemini.AiError) as ctx:
                gemini.parse_response(bad)
            self.assertEqual(ctx.exception.code, code)

    async def test_http_status_mapping_and_key_header(self):
        seen = {}

        def handler(request):
            seen["key"] = request.headers.get("x-goog-api-key")
            seen["url"] = str(request.url)
            return httpx.Response(200, json=gemini_reply("Năm ngón, năm niềm vui"))

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            self.assertEqual(
                await gemini.meaning(self.cfg, 5, b"jpg", client), "Năm ngón, năm niềm vui"
            )
        self.assertEqual(seen["key"], "test-key")
        self.assertIn("gemini-2.5-flash:generateContent", seen["url"])
        self.assertNotIn("test-key", seen["url"])  # key nằm ở header, không ở URL

        for status, code in [(429, "quota"), (403, "auth"), (404, "config"), (500, "network")]:
            quota.clear()
            transport = httpx.MockTransport(lambda r, s=status: httpx.Response(s, json={}))
            async with httpx.AsyncClient(transport=transport) as client:
                with self.assertRaises(gemini.AiError) as ctx:
                    await gemini.meaning(self.cfg, 1, b"", client)
                self.assertEqual(ctx.exception.code, code)

        def slow(request):
            raise httpx.ReadTimeout("slow", request=request)

        async with httpx.AsyncClient(transport=httpx.MockTransport(slow)) as client:
            with self.assertRaises(gemini.AiError) as ctx:
                await gemini.meaning(self.cfg, 1, b"", client)
            self.assertEqual(ctx.exception.code, "timeout")

    async def test_fallback_model_on_temporary_errors(self):
        urls = []

        def handler(request):
            urls.append(str(request.url))
            if "gemini-2.5-flash:" in str(request.url):
                return httpx.Response(429, json={})
            return httpx.Response(200, json=gemini_reply("Câu từ model dự phòng"))

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            text, model = await gemini.meaning_with_fallback(self.cfg, 2, b"", client)
        self.assertEqual((text, model), ("Câu từ model dự phòng", "gemini-lite-test"))
        self.assertEqual(len(urls), 2)

        quota.clear()  # model chính hết lượt -> được nhớ; xóa để thử trường hợp model chính chạy tốt
        async with httpx.AsyncClient(
            transport=httpx.MockTransport(lambda r: httpx.Response(200, json=gemini_reply("A")))
        ) as client:
            self.assertEqual(
                await gemini.meaning_with_fallback(self.cfg, 2, b"", client),
                ("A", "gemini-2.5-flash"),
            )

    async def test_exhausted_model_is_skipped_until_reset(self):
        calls = []

        def handler(request):
            calls.append(str(request.url))
            if "gemini-2.5-flash:" in str(request.url):
                return httpx.Response(429, json={"error": {"details": [{"retryDelay": "82440s"}]}})
            return httpx.Response(200, json=gemini_reply("Câu"))

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            await gemini.meaning_with_fallback(self.cfg, 2, b"", client)
            self.assertEqual(len(calls), 2)
            self.assertGreater(quota.status()["gemini-2.5-flash"], 80000)
            text, model = await gemini.meaning_with_fallback(self.cfg, 2, b"", client)
        self.assertEqual(model, "gemini-lite-test")
        self.assertEqual(len(calls), 3)  # lần 2 không gọi thử model đã hết lượt

    def test_retry_after_parsing(self):
        good = httpx.Response(429, json={"error": {"details": [{"retryDelay": "120s"}]}})
        self.assertEqual(quota.retry_after(good), 120)
        self.assertEqual(quota.retry_after(httpx.Response(429, text="oops")), quota.MIN_BLOCK)

    async def test_no_fallback_for_auth_or_when_disabled(self):
        for cfg, status, code in [
            (self.cfg, 403, "auth"),
            (replace(self.cfg, fallback_model=""), 429, "quota"),
        ]:
            calls = []

            def handler(request, s=status):
                calls.append(1)
                return httpx.Response(s, json={})

            async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
                with self.assertRaises(gemini.AiError) as ctx:
                    await gemini.meaning_with_fallback(cfg, 2, b"", client)
            self.assertEqual(ctx.exception.code, code)
            self.assertEqual(len(calls), 1)

    async def test_both_models_fail(self):
        transport = httpx.MockTransport(lambda r: httpx.Response(503, json={}))
        async with httpx.AsyncClient(transport=transport) as client:
            with self.assertRaises(gemini.AiError) as ctx:
                await gemini.meaning_with_fallback(self.cfg, 2, b"", client)
        self.assertIn("gemini-lite-test", ctx.exception.detail)

    def test_gemini3_primary_uses_thinking_level_fallback_stays_minimal(self):
        cfg = replace(self.cfg, model="gemini-3.5-flash", fallback_model="gemini-3.5-flash-lite")
        primary = gemini.build_request(cfg, 2, None)
        self.assertEqual(primary["generationConfig"]["thinkingConfig"], {"thinkingLevel": "low"})
        fallback = gemini.build_request(cfg, 2, None, "gemini-3.5-flash-lite")
        self.assertNotIn("thinkingConfig", fallback["generationConfig"])
        off = gemini.build_request(replace(cfg, thinking_level=""), 2, None)
        self.assertNotIn("thinkingConfig", off["generationConfig"])

    def test_model_id_accepts_display_names(self):
        self.assertEqual(config.model_id("Gemini 3.5 Flash"), "gemini-3.5-flash")
        self.assertEqual(config.model_id("models/gemini-3.5-flash-lite"), "gemini-3.5-flash-lite")
        self.assertIsNotNone(config.text_model_problem(config.model_id("Gemini-3.8 Flash TTS")))
        self.assertIsNone(config.text_model_problem("gemini-3.5-flash-lite"))

    def test_fallback_config_values(self):
        self.assertEqual(config._fallback(None), config.DEFAULT_FALLBACK_MODEL)
        self.assertEqual(config._fallback(" none "), "")
        self.assertEqual(config._fallback("gemini-x"), "gemini-x")

    async def test_missing_key(self):
        with self.assertRaises(gemini.AiError) as ctx:
            await gemini.meaning(replace(self.cfg, api_key=""), 1, b"")
        self.assertEqual(ctx.exception.code, "config")


class Tts(unittest.TestCase):
    def setUp(self):
        quota.clear()
        self.addCleanup(quota.clear)

    def test_wav_checks_and_cache_path(self):
        self.assertEqual(tts.check_wav(WAV), WAV)
        with self.assertRaises(tts.TtsError):
            tts.check_wav(b"ID3 mp3 data" * 10)
        with self.assertRaises(tts.TtsError):
            tts.check_wav(WAV + b"\x00" * tts.MAX_WAV_BYTES)
        with tempfile.TemporaryDirectory() as directory:
            cfg = make_cfg(directory)
            self.assertEqual(tts.cache_path(cfg, "a"), tts.cache_path(cfg, "a"))
            self.assertNotEqual(tts.cache_path(cfg, "a"), tts.cache_path(cfg, "b"))
            # đổi lời dặn giọng -> tạo giọng mới, không dùng lại bản cache cũ
            self.assertNotEqual(
                tts.cache_path(cfg, "a"), tts.cache_path(replace(cfg, tts_style=""), "a")
            )

    def test_style_config(self):
        self.assertEqual(config._style(None), config.DEFAULT_TTS_STYLE)
        self.assertEqual(config._style("Giọng Huế:"), "Giọng Huế")
        self.assertEqual(config._style("none"), "")

    def test_provider_order_and_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            cfg = make_cfg(directory)
            calls = []

            async def gemini_down(text):
                calls.append("gemini")
                raise tts.TtsError("gemini_http_503")

            async def edge_ok(text):
                calls.append("edge")
                return WAV

            self.assertEqual(
                asyncio.run(tts.speak(cfg, "Câu", {"gemini": gemini_down, "edge": edge_ok})), WAV
            )
            self.assertEqual(calls, ["gemini", "edge"])
            self.assertTrue(tts.cache_path(cfg, "Câu").is_file())  # lần sau lấy cache

            async def edge_down(text):
                raise tts.TtsError("edge_NoAudioReceived")

            with self.assertRaises(tts.TtsError) as ctx:
                asyncio.run(tts.speak(cfg, "Khác", {"gemini": gemini_down, "edge": edge_down}))
            self.assertIn("gemini_http_503", str(ctx.exception))
            self.assertIn("edge_NoAudioReceived", str(ctx.exception))

    def test_gemini_tts_rotates_models_on_quota(self):
        with tempfile.TemporaryDirectory() as directory:
            cfg = make_cfg(directory)
            calls = []

            def handler(request):
                model = str(request.url).split("/models/")[1].split(":")[0]
                calls.append(model)
                if model == "tts-a":
                    return httpx.Response(
                        429, json={"error": {"details": [{"retryDelay": "3600s"}]}}
                    )
                data = base64.b64encode(WAV).decode()
                part = {"inlineData": {"mimeType": "audio/wav", "data": data}}
                return httpx.Response(200, json={"candidates": [{"content": {"parts": [part]}}]})

            async def run():
                async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
                    first = await tts.gemini_tts_any(cfg, "Một", client)
                    second = await tts.gemini_tts_any(cfg, "Hai", client)
                    return first, second

            self.assertEqual(asyncio.run(run()), (WAV, WAV))
            self.assertEqual(calls, ["tts-a", "tts-b", "tts-b"])  # tts-a hết lượt: lần 2 bỏ qua

    def test_long_audio_is_downsampled_to_fit(self):
        long = tts.pcm_to_wav(b"\x10\x00" * 24000 * 12, 24000)  # 12 s ở 24 kHz ~ 576 KB
        self.assertGreater(len(long), tts.MAX_WAV_BYTES)
        out = tts.fit(long)
        self.assertLessEqual(len(out), tts.MAX_WAV_BYTES)
        import io
        import wave

        with wave.open(io.BytesIO(out)) as w:
            self.assertEqual((w.getframerate(), w.getnchannels(), w.getsampwidth()), (16000, 1, 2))
            self.assertAlmostEqual(w.getnframes() / 16000, 12, delta=0.1)
        with self.assertRaises(tts.TtsError):
            tts.fit(tts.pcm_to_wav(b"\x00\x00" * 16000 * 20, 16000))  # 20 s: quá dài

    def test_gemini_tts_parses_wav_and_raw_pcm(self):
        with tempfile.TemporaryDirectory() as directory:
            cfg = make_cfg(directory)

            def reply(mime, data):
                return httpx.Response(
                    200,
                    json={
                        "candidates": [
                            {
                                "content": {
                                    "parts": [
                                        {
                                            "inlineData": {
                                                "mimeType": mime,
                                                "data": base64.b64encode(data).decode(),
                                            }
                                        }
                                    ]
                                }
                            }
                        ]
                    },
                )

            async def run(handler):
                async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
                    return await tts.gemini_tts(cfg, "Xin chào", client)

            sent = []

            def capture(request):
                sent.append(json.loads(request.content)["contents"][0]["parts"][0]["text"])
                return reply("audio/wav", WAV)

            self.assertEqual(asyncio.run(run(capture)), WAV)
            self.assertEqual(sent, ["Giọng nữ Hà Nội, nói nhanh, liền mạch: Xin chào"])
            pcm = asyncio.run(
                run(lambda r: reply("audio/L16;codec=pcm;rate=24000", b"\x01\x00" * 100))
            )
            self.assertEqual(pcm[:4], b"RIFF")
            self.assertEqual(len(pcm), 44 + 200)
            with self.assertRaises(tts.TtsError) as ctx:
                asyncio.run(run(lambda r: httpx.Response(429, json={})))
            self.assertEqual(str(ctx.exception), "http_429")

    def test_cached_wav_needs_no_network(self):
        with tempfile.TemporaryDirectory() as directory:
            cfg = make_cfg(directory)
            path = tts.cache_path(cfg, "Xin chào")
            path.parent.mkdir(parents=True)
            path.write_bytes(WAV)
            self.assertEqual(asyncio.run(tts.speak(cfg, "Xin chào")), WAV)


class MeaningApi(unittest.TestCase):
    def setUp(self):
        quota.clear()
        self.addCleanup(quota.clear)
        self.tmp = tempfile.TemporaryDirectory()
        self.calls = []

    def tearDown(self):
        self.tmp.cleanup()

    def client(self, think=None, **changes):
        async def default_think(count, image):
            self.calls.append((count, len(image)))
            return f"{count} ngón tay 🎉 mang lại niềm vui"

        async def speak(text):
            return WAV

        cfg = make_cfg(self.tmp.name, **changes)
        return TestClient(create_app(cfg, think or default_think, speak))

    def body(self, **extra):
        data = {
            "request_id": "r1",
            "finger_count": 3,
            "source": "esp32",
            "image_b64": base64.b64encode(jpeg()).decode(),
        }
        data.update(extra)
        return data

    def test_ok_shrinks_image_and_cleans_text(self):
        with self.client() as c:
            response = c.post("/v1/meaning", json=self.body())
            self.assertEqual(response.status_code, 200)
            data = response.json()
            self.assertEqual(data["text"], "3 ngón tay mang lại niềm vui")
            self.assertEqual(base64.b64decode(data["audio_wav_b64"]), WAV)
            self.assertTrue(data["image_sent"])
            self.assertGreater(self.calls[0][1], 0)
            self.assertLess(self.calls[0][1], len(jpeg()))  # đã thu nhỏ trước khi gửi
            health = c.get("/health").json()
            self.assertTrue(health["ready"])
            self.assertEqual(health["used_today"], 1)
            self.assertNotIn("test-key", json.dumps(health))
            recent = c.get("/v1/recent").json()["items"]
            self.assertEqual(recent[0]["ok"], 1)

    def test_reports_model_actually_used(self):
        async def lite(count, image):
            return "Câu dự phòng", "gemini-lite-test"

        with self.client(think=lite) as c:
            data = c.post("/v1/meaning", json=self.body()).json()
            self.assertEqual(data["model"], "gemini-lite-test")
            self.assertTrue(data["fallback"])
            self.assertEqual(c.get("/v1/recent").json()["items"][0]["model"], "gemini-lite-test")
            self.assertEqual(c.get("/health").json()["fallback_model"], "gemini-lite-test")

    def test_replay_library(self):
        with self.client() as c:  # chạy thật 1 lần -> câu được lưu vào thư viện
            live = c.post("/v1/meaning", json=self.body()).json()
            self.assertEqual(c.get("/health").json()["library"], {"3": 1})
        self.calls.clear()

        with self.client(
            provider="replay", api_key=""
        ) as c:  # test: không gọi Gemini, không cần key
            self.assertTrue(c.get("/health").json()["ready"])
            data = c.post("/v1/meaning", json=self.body()).json()
            self.assertTrue(data["replay"])
            self.assertEqual(data["text"], live["text"])
            self.assertEqual(base64.b64decode(data["audio_wav_b64"]), WAV)
            self.assertEqual(c.post("/v1/meaning", json=self.body(finger_count=7)).status_code, 503)
            self.assertEqual(c.get("/health").json()["used_today"], 1)  # phát lại không tốn lượt
        self.assertEqual(self.calls, [])

    def test_replay_fallback_when_gemini_fails(self):
        with self.client() as c:
            c.post("/v1/meaning", json=self.body())

        async def broken(count, image):
            raise gemini.AiError("quota", "HTTP 429")

        with self.client(think=broken, replay_fallback=True) as c:
            data = c.post("/v1/meaning", json=self.body()).json()
            self.assertTrue(data["replay"])
            self.assertEqual(c.get("/v1/recent").json()["items"][0]["error"], "quota")
            # chưa có câu cho 9 ngón -> vẫn báo lỗi để core dùng câu cố định
            self.assertEqual(c.post("/v1/meaning", json=self.body(finger_count=9)).status_code, 429)

    def test_send_image_off_sends_number_only(self):
        with self.client(send_image=False) as c:
            self.assertEqual(c.post("/v1/meaning", json=self.body()).status_code, 200)
        self.assertEqual(self.calls[0], (3, 0))

    def test_rejects_bad_input(self):
        with self.client() as c:
            self.assertEqual(c.post("/v1/meaning", json=self.body(finger_count=0)).status_code, 422)
            self.assertEqual(
                c.post("/v1/meaning", json=self.body(finger_count=11)).status_code, 422
            )
            self.assertEqual(
                c.post("/v1/meaning", json=self.body(image_b64="@@not-base64@@")).status_code, 422
            )

    def test_no_key_and_daily_limit(self):
        with self.client(api_key="") as c:
            self.assertEqual(c.post("/v1/meaning", json=self.body()).status_code, 503)
        with self.client(daily_limit=1) as c:
            self.assertEqual(c.post("/v1/meaning", json=self.body()).status_code, 200)
            self.assertEqual(c.post("/v1/meaning", json=self.body()).status_code, 429)

    def test_gemini_error_is_logged_and_mapped(self):
        async def broken(count, image):
            raise gemini.AiError("quota", "HTTP 429")

        with self.client(think=broken) as c:
            response = c.post("/v1/meaning", json=self.body())
            self.assertEqual(response.status_code, 429)
            self.assertEqual(response.json()["detail"], "quota")
            self.assertEqual(c.get("/v1/recent").json()["items"][0]["error"], "quota")


if __name__ == "__main__":
    unittest.main()
