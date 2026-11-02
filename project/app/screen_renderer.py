"""Render Vietnamese text to a TFT-sized JPEG on the Pi."""

from io import BytesIO
from pathlib import Path
import os


def screen_jpeg(state, width, height):
    from PIL import Image, ImageDraw, ImageFont

    candidates = [
        os.getenv("TFT_FONT_PATH", ""),
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    ]
    font_path = next((p for p in candidates if p and Path(p).is_file()), None)
    if not font_path:
        raise RuntimeError(
            "Cần font tiếng Việt: sudo apt install fonts-dejavu-core; hoặc đặt TFT_FONT_PATH"
        )
    image = Image.new("RGB", (width, height), "#0a1424")
    draw = ImageDraw.Draw(image)

    def font(size):
        return ImageFont.truetype(font_path, size)

    small = font(max(12, min(width, height) // 22))
    draw.text(
        (14, 10), f"{state['device_id']} · {state['mode'].upper()}", font=small, fill="#6de2bf"
    )
    if state["view"] == "result" and state["result"]:
        text = state["result"].get("text") or "Chưa nhận diện được cử chỉ"
        title = "Ý nghĩa"
    elif state["view"] == "home":
        title = "Gesture to Speech"
        text = "Chọn Auto hoặc Manual để bắt đầu"
    elif state["view"] == "paused":
        title = "Đã dừng camera"
        text = "Bấm Select để nhận diện ảnh đã dừng hoặc mở menu để tiếp tục"
    else:
        title = "Đang chờ camera"
        text = "Giữ cử chỉ rõ trong khung hình"
    draw.text((14, 40), title, font=small, fill="#a9bdd3")
    lines, selected = [], font(18)
    for size in range(min(42, height // 6), 15, -1):
        selected = font(size)
        lines = []
        line = ""
        for word in text.split():
            trial = (line + " " + word).strip()
            if draw.textlength(trial, font=selected) <= width - 28 or not line:
                line = trial
            else:
                lines.append(line)
                line = word
        if line:
            lines.append(line)
        if len(lines) * (size + 7) <= height - 110:
            break
    y = max(70, (height - 35 - len(lines) * (selected.size + 7)) // 2)
    for line in lines:
        draw.text((14, y), line, font=selected, fill="white")
        y += selected.size + 7
    draw.line((12, height - 33, width - 12, height - 33), fill="#2b3f58")
    draw.text(
        (14, height - 27), "HOME: Menu    UP / DOWN    SELECT: Tiếp tục", font=small, fill="#6de2bf"
    )
    result = BytesIO()
    image.save(result, format="JPEG", quality=90)
    return result.getvalue()
