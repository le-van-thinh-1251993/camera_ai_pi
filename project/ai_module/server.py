"""HTTP API của module AI (chỉ nghe 127.0.0.1). Core gọi POST /v1/meaning khi bật chế độ AI.

GET  /health        trạng thái (có key chưa, model, lượt đã dùng hôm nay) - không lộ key
POST /v1/meaning    {request_id, finger_count 1-10, source, image_b64} ->
                    {text, audio_wav_b64, model, latency_ms, image_sent}
GET  /v1/recent     20 lần gọi gần nhất (để xem/debug, không có ảnh)

AI_PROVIDER=replay: không gọi Google, phát lại câu + giọng đã lưu từ các lần chạy thật (thư viện
theo số ngón). AI_REPLAY_FALLBACK=1: Gemini lỗi/hết lượt cũng phát lại thay vì để core dùng câu cố định.
"""

import asyncio
import base64
import binascii
import logging
import time
from io import BytesIO

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

from ai_module import config, gemini, quota, tts
from ai_module.store import Store
from ai_module.text import clean

log = logging.getLogger("ai-camera.ai")


class MeaningIn(BaseModel):
    request_id: str = Field(default="", max_length=64)
    finger_count: int = Field(ge=1, le=10)
    source: str = Field(default="", max_length=16)
    image_b64: str = Field(default="", max_length=24_000_000)


def shrink(raw, side):
    """Thu nhỏ ảnh còn cạnh dài <= side (đỡ tốn lượt gửi và thời gian Gemini)."""
    from PIL import Image, ImageOps

    with Image.open(BytesIO(raw)) as image:
        image = ImageOps.exif_transpose(image).convert("RGB")
        image.thumbnail((side, side))
        out = BytesIO()
        image.save(out, format="JPEG", quality=85)
        return out.getvalue()


def create_app(cfg=None, think=None, speak=None):
    """think/speak thay được khi test (giả lập Gemini và TTS)."""
    cfg = cfg or config.load()
    think = think or (lambda count, image: gemini.meaning_with_fallback(cfg, count, image))
    speak = speak or (lambda text: tts.speak(cfg, text))
    store = Store(cfg.data_dir / "ai.sqlite3")
    app = FastAPI(title="AI Camera AI module", docs_url=None, redoc_url=None, openapi_url=None)

    @app.get("/health")
    async def health():
        return {
            "ok": True,
            "ready": cfg.ready,
            "provider": cfg.provider,
            "replay_fallback": cfg.replay_fallback,
            "library": store.library_counts(),
            "model": cfg.model,
            "fallback_model": cfg.fallback_model or None,
            "warnings": [
                w
                for w in (
                    config.text_model_problem(cfg.model),
                    cfg.fallback_model and config.text_model_problem(cfg.fallback_model),
                )
                if w
            ],
            "send_image": cfg.send_image,
            "tts": list(cfg.tts_order),
            "tts_models": list(cfg.tts_models),
            "quota_blocked": quota.status(),
            "used_today": store.used_today(),
            "daily_limit": cfg.daily_limit,
        }

    @app.get("/v1/recent")
    async def recent():
        return {"items": store.recent()}

    async def replay(body, started, reason):
        """Phát lại một câu đã lưu cho số ngón này; None nếu thư viện chưa có."""
        item = await asyncio.to_thread(store.pick, body.finger_count)
        if not item:
            return None
        if cfg.replay_delay:
            await asyncio.sleep(cfg.replay_delay)
        text, wav, model = item
        latency = round((time.monotonic() - started) * 1000)
        await asyncio.to_thread(
            store.log,
            body.request_id,
            body.source,
            body.finger_count,
            False,
            True,
            reason,
            latency,
            text,
            "replay:" + (model or ""),
        )
        return {
            "text": text,
            "audio_wav_b64": base64.b64encode(wav).decode("ascii"),
            "model": model,
            "fallback": False,
            "replay": True,
            "latency_ms": latency,
            "image_sent": False,
        }

    @app.post("/v1/meaning")
    async def meaning(body: MeaningIn):
        started = time.monotonic()
        if cfg.provider == "replay":
            answer = await replay(body, started, None)
            if not answer:
                raise HTTPException(503, "replay_empty")
            return answer
        if not cfg.ready:
            raise HTTPException(503, "Chưa có GEMINI_API_KEY trong .env.ai")
        image = b""
        if cfg.send_image and body.image_b64:
            try:
                raw = base64.b64decode(body.image_b64, validate=True)
                image = await asyncio.to_thread(shrink, raw, cfg.image_side)
            except (binascii.Error, ValueError, OSError):
                raise HTTPException(422, "Ảnh không hợp lệ") from None
        if not store.take(cfg.daily_limit):
            answer = cfg.replay_fallback and await replay(body, started, "daily_limit")
            if answer:
                return answer
            raise HTTPException(429, f"Hết hạn mức {cfg.daily_limit} lượt AI hôm nay")
        text, error, model = "", None, cfg.model
        try:
            answer = await think(body.finger_count, image)
            if isinstance(answer, tuple):  # (câu, model đã dùng)
                answer, model = answer
            text = clean(answer, cfg.max_chars)
            if not text:
                raise gemini.AiError("bad_response", "empty_after_clean")
            wav = await speak(text)
        except gemini.AiError as exc:
            error = exc.code
            log.warning("Gemini lỗi %s %s", exc.code, exc.detail)
        except tts.TtsError as exc:
            error = "tts"
            log.warning("TTS lỗi %s", exc)
        except Exception:  # lỗi bất ngờ: vẫn ghi nhật ký, trả 502 cho core lùi về câu cố định
            error = "internal"
            log.exception("Lỗi không lường trước trong /v1/meaning")
        latency = round((time.monotonic() - started) * 1000)
        await asyncio.to_thread(
            store.log,
            body.request_id,
            body.source,
            body.finger_count,
            bool(image),
            error is None,
            error,
            latency,
            text,
            model,
        )
        if error:
            answer = cfg.replay_fallback and await replay(body, started, error)
            if answer:
                return answer
            status = {"quota": 429, "timeout": 504, "auth": 502, "config": 502}.get(error, 502)
            raise HTTPException(status, error)
        await asyncio.to_thread(store.remember, body.finger_count, text, wav, model)
        return {
            "text": text,
            "audio_wav_b64": base64.b64encode(wav).decode("ascii"),
            "model": model,
            "fallback": model != cfg.model,
            "latency_ms": latency,
            "image_sent": bool(image),
        }

    return app
