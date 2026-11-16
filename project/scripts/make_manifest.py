"""Tao lai / kiem tra MANIFEST.sha256.json (SHA-256 cua cac file trong ban phat hanh).

Danh sach file lay tu `git ls-files` (chi file da commit/add), gom:
  project/**, migrations/**, reports/**, .github/** va cac file goc cua bo cai dat.
Khong gom: anh bo mach/, voice/, ghi chu ca nhan, *.ps1 (chi dung tren Windows).
File van ban duoc doi CRLF -> LF truoc khi bam, nen ket qua giong nhau tren Windows va Pi.

  python project/scripts/make_manifest.py           # ghi lai MANIFEST.sha256.json
  python project/scripts/make_manifest.py --check   # chi kiem tra (CI), loi neu lech
"""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MANIFEST = REPO / "MANIFEST.sha256.json"
PREFIXES = ("project/", "migrations/", "reports/", ".github/")
ROOT_FILES = {
    "README.md",
    "REVIEW_FIXES.md",
    "VALIDATION.md",
    "install.py",
    "install_pi.sh",
    "cloud.env.example",
    ".env.cloud.example",
}
BINARY = {".jpg", ".jpeg", ".png", ".wav", ".mp3", ".rar", ".zip", ".xlsx", ".task", ".tflite"}


def tracked():
    out = subprocess.run(
        ["git", "ls-files", "-z"], cwd=REPO, capture_output=True, check=True
    ).stdout
    return sorted(p for p in out.decode("utf-8").split("\0") if p)


def included(path):
    if path.endswith(".ps1") or path == MANIFEST.name:
        return False
    return path in ROOT_FILES or path.startswith(PREFIXES)


def digest(path):
    data = (REPO / path).read_bytes()
    if Path(path).suffix.lower() not in BINARY:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def build():
    return {p: digest(p) for p in tracked() if included(p) and (REPO / p).is_file()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="chi kiem tra, khong ghi")
    args = ap.parse_args()
    sums = build()
    if args.check:
        old = json.loads(MANIFEST.read_text(encoding="utf-8")) if MANIFEST.is_file() else {}
        diff = sorted(p for p in set(old) | set(sums) if old.get(p) != sums.get(p))
        if diff:
            print("MANIFEST.sha256.json lech", len(diff), "file:")
            for p in diff:
                print("  ", p)
            print("Chay: python project/scripts/make_manifest.py")
            sys.exit(1)
        print("MANIFEST.sha256.json khop", len(sums), "file")
        return
    with MANIFEST.open("w", encoding="utf-8", newline="\n") as out:
        out.write(json.dumps(sums, ensure_ascii=False, indent=2) + "\n")
    print("Da ghi", MANIFEST.name, len(sums), "file")


if __name__ == "__main__":
    main()
