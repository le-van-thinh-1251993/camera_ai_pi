"""Tao tai nguyen cho firmware TFT: font tieng Viet (VLW) va am thanh chao.

    python scripts/make_tft_assets.py                 # font + recordings/xinchao.wav
    python scripts/make_tft_assets.py --wav loi_chao.wav

Ghi ra firmware/HybridTerminal/fonts_vi.h va welcome_audio.h.
Font mac dinh: Noto Sans (SIL OFL) trong Windows, hoac DejaVu Sans neu khong co.
WAV phai la mono 16-bit PCM, 8-48 kHz (giong file Pi tra ve).
"""

from __future__ import annotations

import argparse
import struct
import sys
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "firmware" / "HybridTerminal"

VI_LOWER = "àáảãạăằắẳẵặâầấẩẫậèéẻẽẹêềếểễệìíỉĩịòóỏõọôồốổỗộơờớởỡợùúủũụưừứửữựỳýỷỹỵđ"
FULL = "".join(chr(c) for c in range(0x21, 0x7F)) + VI_LOWER + VI_LOWER.upper() + "…–—°×·"

# (ten C, kieu, co chu px, bo ky tu)
FONTS = [
    ("FONT_VI_12B", "bold", 12, FULL),
    ("FONT_VI_15", "regular", 15, FULL),
    ("FONT_VI_15B", "bold", 15, FULL),
    ("FONT_VI_17", "regular", 17, FULL),
    ("FONT_VI_13B", "bold", 13, FULL),
    ("FONT_VI_24B", "bold", 24, FULL),
    ("FONT_VI_28B", "bold", 28, FULL),
    ("FONT_VI_56B", "bold", 56, "Xin chào!"),
]

CANDIDATES = {
    "regular": [
        Path("C:/Windows/Fonts/NotoSans-Regular.ttf"),
        Path("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"),
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
    ],
    "bold": [
        Path("C:/Windows/Fonts/NotoSans-Bold.ttf"),
        Path("/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"),
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"),
    ],
}


def find_font(style: str, override: str | None) -> Path:
    if override:
        return Path(override)
    for path in CANDIDATES[style]:
        if path.is_file():
            return path
    try:
        import matplotlib

        name = "DejaVuSans-Bold.ttf" if style == "bold" else "DejaVuSans.ttf"
        path = Path(matplotlib.get_data_path()) / "fonts" / "ttf" / name
        if path.is_file():
            return path
    except ImportError:
        pass
    sys.exit(f"Khong tim thay font {style}; dung --regular/--bold")


def vlw(font_path: Path, size: int, chars: str) -> bytes:
    """Font VLW (Processing/TFT_eSPI) ma LovyanGFX loadFont() doc duoc."""
    font = ImageFont.truetype(str(font_path), size)
    ascent, descent = font.getmetrics()
    codes = sorted({ord(c) for c in chars if c != " "})
    metrics, bitmaps = [], []
    for code in codes:
        ch = chr(code)
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        advance = round(font.getlength(ch))
        if w and h:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            bitmaps.append(img.tobytes())
        else:
            bitmaps.append(b"")
        if w > 255 or advance > 255:
            sys.exit(f"Ky tu {ch!r} qua rong cho VLW")
        metrics.append(struct.pack(">7i", code, h, w, advance, -y0, x0, 0))
    header = struct.pack(">6i", len(codes), 11, size, 0, ascent, descent)
    return header + b"".join(metrics) + b"".join(bitmaps)


def c_array(name: str, data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 24):
        lines.append("  " + ",".join(f"0x{b:02x}" for b in data[i : i + 24]) + ",")
    body = "\n".join(lines)
    return f"alignas(4) static const uint8_t {name}[{len(data)}] PROGMEM = {{\n{body}\n}};\n"


def write_fonts(regular: Path, bold: Path) -> None:
    parts = [
        "#pragma once\n",
        "// TU DONG TAO boi scripts/make_tft_assets.py - khong sua tay.\n",
        f"// Font: {regular.name} / {bold.name}\n",
        "#include <Arduino.h>\n\n",
    ]
    total = 0
    for name, style, size, chars in FONTS:
        data = vlw(bold if style == "bold" else regular, size, chars)
        total += len(data)
        parts.append(c_array(name, data) + "\n")
        print(f"{name}: {len(data) / 1024:.1f} KB")
    (OUT_DIR / "fonts_vi.h").write_text("".join(parts), encoding="utf-8", newline="\n")
    print(f"fonts_vi.h: {total / 1024:.1f} KB")


def write_wav(path: Path) -> None:
    with wave.open(str(path)) as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or not 8000 <= w.getframerate() <= 48000:
            sys.exit("WAV phai la mono, 16-bit, 8-48 kHz")
    data = path.read_bytes()
    text = (
        "#pragma once\n"
        "// TU DONG TAO boi scripts/make_tft_assets.py - khong sua tay.\n"
        f"// Nguon: {path.name}\n"
        "#include <Arduino.h>\n\n" + c_array("WELCOME_WAV", data)
    )
    (OUT_DIR / "welcome_audio.h").write_text(text, encoding="utf-8", newline="\n")
    print(f"welcome_audio.h: {len(data) / 1024:.1f} KB tu {path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--wav", default=str(ROOT / "recordings" / "xinchao.wav"))
    parser.add_argument("--regular")
    parser.add_argument("--bold")
    parser.add_argument("--skip-fonts", action="store_true")
    args = parser.parse_args()
    if not args.skip_fonts:
        write_fonts(find_font("regular", args.regular), find_font("bold", args.bold))
    write_wav(Path(args.wav))


if __name__ == "__main__":
    main()
