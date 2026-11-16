"""Cấu hình module AI, đọc từ .env.ai (cạnh .env của core). Không in key ra log."""

import os
import re
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv

ROOT = Path(__file__).resolve().parents[1]
ENV_FILE = Path(os.getenv("AI_ENV_FILE", str(ROOT / ".env.ai")))
DEFAULT_MODEL = "gemini-3.5-flash"
DEFAULT_FALLBACK_MODEL = "gemini-3.5-flash-lite"
DEFAULT_STYLE = "vui vẻ, tích cực, ấm áp, giống lời chúc ngắn"


def _int(name, default, low, high):
    try:
        return max(low, min(high, int(os.getenv(name, str(default)))))
    except ValueError:
        return default


def model_id(value):
    """Chấp nhận cả tên hiển thị ("Gemini 3.5 Flash") -> mã model ("gemini-3.5-flash")."""
    value = re.sub(r"\s+", "-", (value or "").strip().lower())
    return value.removeprefix("models/")


def text_model_problem(model):
    """Model chỉ tạo giọng/ảnh thì không nghĩ câu được."""
    if any(tag in model.split("-") for tag in ("tts", "image", "embedding")):
        return f"{model} không phải model tạo văn bản"
    return None


DEFAULT_TTS_STYLE = "Giọng nữ Hà Nội, nói nhanh, liền mạch"
# Đo thật 11/2026 với lời dặn trên: cả hai cho giọng nữ miền Bắc, tự nhiên 10/10, không đọc
# lời dặn. KHÔNG dùng gemini-3.8-flash-lite-tts: model này đọc luôn cả lời dặn ra loa.
DEFAULT_TTS_MODELS = ("gemini-3.8-flash-tts", "gemini-3.1-flash-tts-preview")


def _style(value):
    value = (value or "").strip().rstrip(":").strip()
    if value.lower() == "none":
        return ""
    return value or DEFAULT_TTS_STYLE


def _fallback(value):
    """Model dự phòng; "none"/"off"/"0" = tắt."""
    value = (value or "").strip()
    if value.lower() in ("none", "off", "0"):
        return ""
    return model_id(value) or DEFAULT_FALLBACK_MODEL


def _float(name, default, low, high):
    try:
        return max(low, min(high, float(os.getenv(name, str(default)))))
    except ValueError:
        return default


@dataclass(frozen=True)
class Config:
    api_key: str
    provider: str
    replay_fallback: bool
    replay_delay: float
    model: str
    fallback_model: str
    thinking_budget: str
    thinking_level: str
    style: str
    max_chars: int
    daily_limit: int
    send_image: bool
    image_side: int
    gemini_timeout: float
    fallback_timeout: float
    tts_order: tuple
    tts_models: tuple
    tts_voice: str
    tts_style: str
    tts_timeout: float
    voice: str
    voice_rate: str
    data_dir: Path
    host: str
    port: int

    @property
    def ready(self):
        return self.provider == "replay" or bool(self.api_key)


def load(env_file=None):
    load_dotenv(env_file or ENV_FILE, override=False)
    data = Path(os.getenv("AI_DATA_DIR", str(ROOT / "data" / "ai")))
    if not data.is_absolute():
        data = ROOT / data
    return Config(
        api_key=os.getenv("GEMINI_API_KEY", "").strip(),
        # gemini = gọi thật; replay = phát lại câu đã lưu, không gọi Google (test không tốn lượt)
        provider="replay"
        if os.getenv("AI_PROVIDER", "gemini").strip().lower() == "replay"
        else "gemini",
        # Gemini lỗi/hết lượt -> phát lại câu đã lưu cùng số ngón thay vì câu cố định
        replay_fallback=os.getenv("AI_REPLAY_FALLBACK", "1") != "0",
        # Giây chờ giả khi phát lại (giống thời gian chạy thật nếu muốn)
        replay_delay=_float("AI_REPLAY_DELAY", 0, 0, 15),
        model=model_id(os.getenv("GEMINI_MODEL")) or DEFAULT_MODEL,
        fallback_model=_fallback(os.getenv("GEMINI_FALLBACK_MODEL")),
        thinking_budget=os.getenv("GEMINI_THINKING_BUDGET", "0").strip(),
        thinking_level=os.getenv("GEMINI_THINKING_LEVEL", "low").strip().lower(),
        style=os.getenv("AI_STYLE", "").strip() or DEFAULT_STYLE,
        max_chars=_int("AI_MAX_CHARS", 100, 30, 160),
        daily_limit=_int("AI_DAILY_LIMIT", 500, 1, 100000),
        send_image=os.getenv("AI_SEND_IMAGE", "1") != "0",
        image_side=_int("AI_IMAGE_SIDE", 768, 256, 1600),
        gemini_timeout=_float("GEMINI_TIMEOUT", 6, 2, 30),
        fallback_timeout=_float("GEMINI_FALLBACK_TIMEOUT", 4, 2, 30),
        tts_order=tuple(
            p
            for p in (s.strip().lower() for s in os.getenv("AI_TTS", "gemini,edge").split(","))
            if p in ("gemini", "edge")
        )
        or ("gemini", "edge"),
        # Danh sách model giọng, thử lần lượt (mỗi model có hạn mức Free riêng).
        tts_models=tuple(
            m for m in (model_id(s) for s in os.getenv("GEMINI_TTS_MODEL", "").split(",")) if m
        )
        or DEFAULT_TTS_MODELS,
        tts_voice=os.getenv("GEMINI_TTS_VOICE", "").strip() or "Kore",
        # Gemini TTS không có sẵn giọng miền Bắc: điều khiển bằng lời dặn (đo thật 11/2026: Kore +
        # lời dặn này = nữ, miền Bắc, tự nhiên 10/10, không đọc lời dặn ra). "none" = không dặn.
        tts_style=_style(os.getenv("GEMINI_TTS_STYLE")),
        tts_timeout=_float("GEMINI_TTS_TIMEOUT", 10, 3, 30),
        voice=os.getenv("AI_VOICE", "vi-VN-HoaiMyNeural").strip(),
        voice_rate=os.getenv("AI_VOICE_RATE", "+0%").strip(),
        data_dir=data.resolve(),
        host=os.getenv("AI_HOST", "127.0.0.1"),
        port=_int("AI_PORT", 8010, 1, 65535),
    )
