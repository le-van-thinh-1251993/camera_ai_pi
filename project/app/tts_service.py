"""Recorded voice by default; optional Edge TTS is normalized to PCM WAV for ESP32."""

import asyncio
import hashlib
import os
from pathlib import Path
from app.recorded_speech import generate as recorded


async def generate(key, text, data_dir):
    if os.getenv("TTS_PROVIDER", "recording") != "edge":
        return await recorded(key, data_dir)
    voice = os.getenv("VOICE", "vi-VN-HoaiMyNeural")
    digest = hashlib.sha256((voice + "|" + text).encode()).hexdigest()
    target = Path(data_dir) / "audio" / (digest + ".wav")
    if target.is_file():
        return "/api/audio/" + target.name, "edge-cache"
    temporary = target.with_suffix(".mp3")
    partial = target.with_suffix(".tmp.wav")
    try:
        import edge_tts

        await asyncio.wait_for(edge_tts.Communicate(text, voice).save(str(temporary)), 25)
        proc = await asyncio.create_subprocess_exec(
            "ffmpeg",
            "-nostdin",
            "-y",
            "-loglevel",
            "error",
            "-i",
            str(temporary),
            "-ac",
            "1",
            "-ar",
            "24000",
            "-c:a",
            "pcm_s16le",
            str(partial),
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
        try:
            code = await asyncio.wait_for(proc.wait(), 15)
        except BaseException:
            proc.kill()
            await proc.wait()
            raise
        if code or not partial.is_file():
            raise RuntimeError("WAV conversion failed")
        partial.replace(target)
        return "/api/audio/" + target.name, "edge"
    except Exception:
        return await recorded(key, data_dir)
    finally:
        temporary.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)
