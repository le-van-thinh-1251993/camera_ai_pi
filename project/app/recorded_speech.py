"""Dùng 12 file người dùng cung cấp, không gọi TTS/API và không đổi giọng."""

from pathlib import Path
import json, hashlib, asyncio, shutil, logging

BANK = Path(__file__).resolve().parent.parent / "recordings"
log = logging.getLogger("ai-camera.recordings")


async def generate(key, data_dir):
    try:
        manifest = json.loads((BANK / "manifest.json").read_text(encoding="utf-8"))
        entry = manifest["clips"][key]
        source = BANK / entry["file"]
        digest = entry["sha256"]
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise ValueError("Hash không hợp lệ")
        target = Path(data_dir) / "audio" / (digest + ".wav")
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                raise ValueError("File thu âm khác checksum")
            await asyncio.to_thread(shutil.copyfile, source, target)
        return "/api/audio/" + target.name, "recording"
    except (OSError, KeyError, ValueError):
        log.exception("Không tìm thấy/không đọc được file thu âm: %s", key)
        return None, "recording-missing"
