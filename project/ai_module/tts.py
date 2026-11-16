"""Giọng đọc cho câu AI -> WAV PCM 16-bit mono (bo ESP32 phát được, tối đa ~500 KB).

Thứ tự nhà cung cấp theo AI_TTS (mặc định "gemini,edge"):
- gemini: Gemini TTS, thử lần lượt các model trong GEMINI_TTS_MODEL (giọng GEMINI_TTS_VOICE + lời dặn
          GEMINI_TTS_STYLE), trả WAV/PCM 24 kHz, không cần ffmpeg. Model hết lượt (429) bị bỏ qua tới
          khi reset.
- edge:   Edge TTS + ffmpeg. Đo thật 11/2026: chỉ 3/12 lần thành công (NoAudioReceived) -> chỉ để dự phòng.
Câu đã đọc được cache theo SHA-256 (nhà cung cấp|giọng|câu), không tạo lại lần hai.
"""

import array
import asyncio
import base64
import hashlib
import io
import re
import shutil
import sys
import wave
from pathlib import Path

import httpx

from ai_module import quota

MAX_WAV_BYTES = 500_000  # firmware: MAX_WAV_BYTES = 512000
API = "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"


class TtsError(Exception):
    pass


def cache_path(cfg, text):
    key = "|".join(
        (
            ",".join(cfg.tts_order),
            ",".join(cfg.tts_models),
            cfg.tts_voice,
            cfg.tts_style,
            cfg.voice,
            cfg.voice_rate,
            text,
        )
    )
    digest = hashlib.sha256(key.encode()).hexdigest()
    return Path(cfg.data_dir) / "tts" / (digest + ".wav")


def check_wav(data):
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise TtsError("not_wav")
    if len(data) > MAX_WAV_BYTES:
        raise TtsError("too_long")
    return data


def pcm_to_wav(pcm, rate):
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return out.getvalue()


def fit(data):
    """WAV quá 500 KB thì hạ xuống 16 kHz (nội suy tuyến tính), vẫn quá thì báo lỗi."""
    if len(data) <= MAX_WAV_BYTES:
        return check_wav(data)
    try:
        with wave.open(io.BytesIO(data)) as w:
            rate, width, channels = w.getframerate(), w.getsampwidth(), w.getnchannels()
            frames = w.readframes(w.getnframes())
    except (wave.Error, EOFError):
        raise TtsError("not_wav") from None
    if width != 2 or channels != 1 or rate <= 16000:
        raise TtsError("too_long")
    src = array.array("h", frames)
    if sys.byteorder == "big":
        src.byteswap()
    step = rate / 16000
    out = array.array("h", bytes(2 * int(len(src) / step)))
    last = len(src) - 1
    for i in range(len(out)):
        pos = i * step
        j = int(pos)
        k = min(j + 1, last)
        out[i] = int(src[j] + (src[k] - src[j]) * (pos - j))
    if sys.byteorder == "big":
        out.byteswap()
    return check_wav(pcm_to_wav(out.tobytes(), 16000))


async def gemini_tts_any(cfg, text, client=None):
    """Thử lần lượt các model giọng, bỏ qua model đã hết lượt (429) tới khi reset."""
    errors = []
    for model in cfg.tts_models:
        if quota.blocked(model):
            errors.append(f"{model}_quota")
            continue
        try:
            return await gemini_tts(cfg, text, client, model)
        except TtsError as exc:
            errors.append(f"{model}_{exc}")
    raise TtsError(",".join(errors) or "gemini_no_model")


async def gemini_tts(cfg, text, client=None, model=None):
    if not cfg.api_key:
        raise TtsError("gemini_no_key")
    model = model or cfg.tts_models[0]
    body = {
        "contents": [{"parts": [{"text": f"{cfg.tts_style}: {text}" if cfg.tts_style else text}]}],
        "generationConfig": {
            "responseModalities": ["AUDIO"],
            "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": cfg.tts_voice}}},
        },
    }
    own = client is None
    client = client or httpx.AsyncClient(timeout=cfg.tts_timeout)
    try:
        response = await client.post(
            API.format(model=model),
            headers={"x-goog-api-key": cfg.api_key},
            json=body,
            timeout=cfg.tts_timeout,
        )
    except httpx.TimeoutException:
        raise TtsError("gemini_timeout") from None
    except httpx.HTTPError as exc:
        raise TtsError("gemini_" + type(exc).__name__) from None
    finally:
        if own:
            await client.aclose()
    if response.status_code == 429:
        quota.block(model, response)
    if response.status_code != 200:
        raise TtsError(f"http_{response.status_code}")
    try:
        part = response.json()["candidates"][0]["content"]["parts"][0]["inlineData"]
        audio = base64.b64decode(part["data"])
        mime = part.get("mimeType", "").lower()
    except (ValueError, KeyError, IndexError, TypeError):
        raise TtsError("gemini_bad_response") from None
    if audio[:4] == b"RIFF":
        return audio
    if "l16" in mime or "pcm" in mime:  # PCM 16-bit little-endian thô
        rate = re.search(r"rate=(\d+)", mime)
        return pcm_to_wav(audio, int(rate.group(1)) if rate else 24000)
    raise TtsError("gemini_unknown_format")


async def edge_tts_wav(cfg, text, target):
    if not shutil.which("ffmpeg"):
        raise TtsError("ffmpeg_missing")
    try:
        import edge_tts
    except ImportError:
        raise TtsError("edge_tts_missing") from None
    target.parent.mkdir(parents=True, exist_ok=True)
    mp3 = target.with_suffix(".mp3")
    partial = target.with_suffix(".tmp.wav")
    try:
        for attempt in range(2):  # Edge hay trả NoAudioReceived ngẫu nhiên: thử lại 1 lần
            try:
                await asyncio.wait_for(
                    edge_tts.Communicate(text, cfg.voice, rate=cfg.voice_rate).save(str(mp3)), 6
                )
                break
            except asyncio.TimeoutError:
                raise TtsError("edge_timeout") from None
            except Exception as exc:  # edge_tts.exceptions.* , aiohttp.*
                if attempt == 1:
                    raise TtsError("edge_" + type(exc).__name__) from None
        proc = await asyncio.create_subprocess_exec(
            "ffmpeg",
            "-nostdin",
            "-y",
            "-loglevel",
            "error",
            "-i",
            str(mp3),
            "-ac",
            "1",
            "-ar",
            "16000",
            "-c:a",
            "pcm_s16le",
            str(partial),
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
        if await asyncio.wait_for(proc.wait(), 15) != 0:
            raise TtsError("ffmpeg_failed")
        return partial.read_bytes()
    except asyncio.TimeoutError:
        raise TtsError("ffmpeg_timeout") from None
    except OSError as exc:
        raise TtsError(type(exc).__name__) from None
    finally:
        mp3.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)


async def speak(cfg, text, providers=None):
    """providers: {"gemini": async fn(text), "edge": async fn(text)} để test."""
    target = cache_path(cfg, text)
    if target.is_file():
        return target.read_bytes()
    providers = providers or {
        "gemini": lambda t: gemini_tts_any(cfg, t),
        "edge": lambda t: edge_tts_wav(cfg, t, target),
    }
    errors = []
    for name in cfg.tts_order:
        if name not in providers:
            continue
        try:
            data = fit(await providers[name](text))
        except TtsError as exc:
            errors.append(str(exc))
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_suffix(".part")
        partial.write_bytes(data)
        partial.replace(target)
        return data
    raise TtsError(";".join(errors) or "no_provider")
