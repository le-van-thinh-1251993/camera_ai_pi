"""Phía core: gọi module AI độc lập (AI_MODULE_URL) để lấy câu ý nghĩa + giọng đọc.

Mọi lỗi (module tắt, mất mạng, hết hạn mức, quá giờ) đều trả None để core dùng câu cố
định và file ghi âm như cũ. Giọng đọc AI được lưu vào DATA/audio/<sha256>.wav nên các
đường sẵn có (LAN /audio.wav, upload Supabase, /api/audio) dùng được không cần sửa.
"""

import asyncio
import base64
import hashlib
import logging
import os
from pathlib import Path

import httpx

log = logging.getLogger("ai-camera.ai-client")
MAX_WAV_BYTES = 500_000


class AiClient:
    def __init__(self, data_dir, transport=None):
        self.url = os.getenv("AI_MODULE_URL", "").strip().rstrip("/")
        try:
            self.timeout = max(2.0, min(30.0, float(os.getenv("AI_TIMEOUT", "20"))))
        except ValueError:
            self.timeout = 20.0
        self.audio_dir = Path(data_dir) / "audio"
        self.last_error = None
        self.transport = transport  # test: httpx.MockTransport

    @property
    def enabled(self):
        return bool(self.url)

    async def meaning(self, request_id, finger_count, source, image):
        """-> {"text", "audio_url", "meta"} hoặc None (đã ghi lý do vào last_error)."""
        if not self.enabled:
            self.last_error = "disabled"
            return None
        try:
            return await asyncio.wait_for(
                self._call(request_id, finger_count, source, image), self.timeout
            )
        except asyncio.TimeoutError:
            self.last_error = "timeout"
        except httpx.HTTPStatusError as exc:
            detail = ""
            try:
                detail = exc.response.json().get("detail", "")
            except ValueError:
                pass
            self.last_error = str(detail or exc.response.status_code)[:60]
        except (httpx.HTTPError, ValueError, KeyError, TypeError, OSError) as exc:
            self.last_error = type(exc).__name__
        log.warning("Module AI không dùng được (%s) - dùng câu cố định", self.last_error)
        return None

    async def _call(self, request_id, finger_count, source, image):
        payload = {
            "request_id": request_id,
            "finger_count": int(finger_count),
            "source": source or "",
            "image_b64": base64.b64encode(image or b"").decode("ascii"),
        }
        async with httpx.AsyncClient(timeout=self.timeout, transport=self.transport) as client:
            response = await client.post(self.url + "/v1/meaning", json=payload)
            response.raise_for_status()
            data = response.json()
        text = str(data["text"]).strip()
        wav = base64.b64decode(data["audio_wav_b64"], validate=True)
        if not text or len(text) > 200:
            raise ValueError("bad_text")
        if len(wav) > MAX_WAV_BYTES or wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
            raise ValueError("bad_wav")
        digest = hashlib.sha256(wav).hexdigest()
        target = self.audio_dir / (digest + ".wav")
        if not target.is_file():
            await asyncio.to_thread(self._write, target, wav)
        self.last_error = None
        return {
            "text": text,
            "audio_url": "/api/audio/" + target.name,
            "meta": {
                "status": "ok",
                "provider": "gemini",
                "model": data.get("model"),
                "fallback": bool(data.get("fallback")),
                "replay": bool(data.get("replay")),
                "latency_ms": data.get("latency_ms"),
                "image_sent": bool(data.get("image_sent")),
            },
        }

    @staticmethod
    def _write(target, wav):
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_suffix(".tmp")
        partial.write_bytes(wav)
        partial.replace(target)
