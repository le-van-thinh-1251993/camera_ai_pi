"""Nhớ model Gemini nào đã hết hạn mức (HTTP 429) để bỏ qua tới khi reset.

Gói Free tính hạn mức theo NGÀY cho từng model (đo thật 11/2026: gemini-3.5-flash 20 lượt,
gemini-3.8-flash-tts 10 lượt). Không nhớ thì mỗi lần nhận diện tốn thêm 1-2 s gọi thử vô ích.
"""

import re
import time

_blocked = {}
MIN_BLOCK = 60
MAX_BLOCK = 24 * 3600


def blocked(model):
    return _blocked.get(model, 0) > time.monotonic()


def retry_after(response):
    """Đọc retryDelay ("82440s") trong lỗi 429 của Gemini; không có thì 60 s."""
    try:
        for detail in response.json().get("error", {}).get("details", []):
            match = re.fullmatch(r"(\d+(?:\.\d+)?)s", str(detail.get("retryDelay", "")))
            if match:
                return float(match.group(1))
    except (ValueError, AttributeError):
        pass
    return MIN_BLOCK


def block(model, response=None):
    seconds = retry_after(response) if response is not None else MIN_BLOCK
    _blocked[model] = time.monotonic() + max(MIN_BLOCK, min(MAX_BLOCK, seconds))


def status():
    now = time.monotonic()
    return {model: round(until - now) for model, until in _blocked.items() if until > now}


def clear():
    _blocked.clear()
