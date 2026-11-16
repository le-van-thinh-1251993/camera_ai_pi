"""Làm sạch câu AI để TFT vẽ được (font chỉ có chữ Latin + tiếng Việt) và loa đọc tự nhiên."""

import re
import unicodedata

_EXTRA = set("…–—°×·")
_QUOTES = {"“": '"', "”": '"', "‘": "'", "’": "'", "«": '"', "»": '"'}


def _allowed(ch):
    if ch == " " or 0x21 <= ord(ch) <= 0x7E or ch in _EXTRA:
        return True
    if not unicodedata.category(ch).startswith("L"):
        return False
    return "LATIN" in unicodedata.name(ch, "")


def clean(text, max_chars):
    """Bỏ emoji/ký hiệu lạ, gộp khoảng trắng, cắt theo từ nếu quá dài."""
    text = unicodedata.normalize("NFC", str(text or ""))
    text = "".join(_QUOTES.get(ch, ch) for ch in text)
    text = re.sub(r"\s+", " ", text)
    text = "".join(ch for ch in text if _allowed(ch))
    text = re.sub(r" {2,}", " ", text).strip(" \"'")
    if len(text) > max_chars:
        cut = text[: max_chars - 1]
        if " " in cut:
            cut = cut[: cut.rfind(" ")]
        text = cut.rstrip(" ,;:-") + "…"
    return text
