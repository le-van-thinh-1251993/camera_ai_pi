"""Nạp bộ voice mới vào recordings/ cho Count_1..Count_10.

Cách dùng (chạy trong thư mục project):
    python scripts/import_voice.py <thư_mục_voice>

Thư mục voice cần có 10 file tên 1..10 (đuôi bất kỳ: .wav, .mp3, .m4a, .ogg...).
Script sẽ:
  - đổi từng file sang WAV PCM16, mono, 24 kHz (định dạng ESP32 phát được) bằng ffmpeg
  - ghi đè recordings/1.wav .. 10.wav
  - cập nhật recordings/manifest.json (sha256, thời lượng, câu nói lấy từ config/gestures.json)
  - cập nhật ../MANIFEST.sha256.json nếu có
Tùy chọn thêm file "greeting" (Xin chào) và "unknown" (chưa nhận diện được);
không có thì các clip Greeting / Unknown / No_hand giữ nguyên.
"""

import hashlib
import json
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BANK = ROOT / "recordings"
PHRASES = json.loads((ROOT / "config/gestures.json").read_text(encoding="utf-8"))
AUDIO_EXT = {".wav", ".mp3", ".m4a", ".aac", ".ogg", ".opus", ".flac", ".webm", ".amr", ""}


def find_source(folder: Path, n: int) -> Path:
    hits = [
        p
        for p in folder.iterdir()
        if p.is_file() and p.stem.strip() == str(n) and p.suffix.lower() in AUDIO_EXT
    ]
    if not hits:
        raise SystemExit(
            f"Thiếu file cho {n} ngón (cần file tên '{n}.wav', '{n}.mp3'...) trong {folder}"
        )
    return hits[0]


def convert(src: Path, dst: Path) -> None:
    tmp = dst.with_suffix(".tmp.wav")
    subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-y",
            "-loglevel",
            "error",
            "-i",
            str(src),
            "-ac",
            "1",
            "-ar",
            "24000",
            "-c:a",
            "pcm_s16le",
            str(tmp),
        ],
        check=True,
    )
    tmp.replace(dst)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    folder = Path(sys.argv[1]).expanduser().resolve()
    manifest_path = BANK / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    sources = {n: find_source(folder, n) for n in range(1, 11)}

    for n, src in sources.items():
        key, dst = f"Count_{n}", BANK / f"{n}.wav"
        convert(src, dst)
        with wave.open(str(dst)) as w:
            duration = round(w.getnframes() / w.getframerate(), 3)
        manifest["clips"][key] = {
            "file": dst.name,
            "sha256": hashlib.sha256(dst.read_bytes()).hexdigest(),
            "text": PHRASES[key],
            "duration_seconds": duration,
            "sample_rate": 24000,
            "channels": 1,
        }
        print(f"{n:>2} ngón  {duration:5.2f}s  {PHRASES[key]}  <- {src.name}")

    # Tùy chọn: file "greeting" (Xin chào) và "unknown" (chưa nhận diện được cử chỉ).
    for stem, wav, keys in (
        ("greeting", "xinchao.wav", ["Greeting"]),
        ("unknown", "chuanhandiencuchi.wav", ["Unknown", "No_hand"]),
    ):
        hits = [p for p in folder.iterdir() if p.is_file() and p.stem.strip().lower() == stem]
        if not hits:
            continue
        dst = BANK / wav
        convert(hits[0], dst)
        with wave.open(str(dst)) as w:
            duration = round(w.getnframes() / w.getframerate(), 3)
        for key in keys:
            manifest["clips"][key] = {
                "file": wav,
                "sha256": hashlib.sha256(dst.read_bytes()).hexdigest(),
                "text": PHRASES[key],
                "duration_seconds": duration,
                "sample_rate": 24000,
                "channels": 1,
            }
        print(f"{stem:>8}  {duration:5.2f}s  {PHRASES[keys[0]]}  <- {hits[0].name}")

    manifest["source"] = f"User supplied {folder.name}"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    release = ROOT.parent / "MANIFEST.sha256.json"
    if release.is_file():
        sums = json.loads(release.read_text(encoding="utf-8"))
        clips = [
            *(BANK / f"{n}.wav" for n in range(1, 11)),
            BANK / "xinchao.wav",
            BANK / "chuanhandiencuchi.wav",
        ]
        for path in [*clips, manifest_path, ROOT / "config/gestures.json"]:
            rel = path.relative_to(ROOT.parent).as_posix()
            if rel in sums:
                sums[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
        release.write_text(json.dumps(sums, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("Xong. Khởi động lại server để dùng voice mới.")


if __name__ == "__main__":
    main()
