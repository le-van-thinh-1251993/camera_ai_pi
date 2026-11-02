"""Update existing Pi source with backups. Existing Supabase and device keys are never rewritten."""

import argparse
import ast
import json
import re
import shutil
import secrets
from datetime import datetime
from io import StringIO
from pathlib import Path

from dotenv import dotenv_values


def restore(root, backup):
    if backup.parent != root / "backups" or not backup.name.startswith(
        ("hybrid-", "cloud-v6-")
    ):
        raise ValueError("Invalid project backup")
    manifest = json.loads((backup / "manifest.json").read_text())
    for name, existed in manifest.items():
        target = (root / name).resolve()
        if not target.is_relative_to(root):
            raise ValueError("Invalid backup path")
        if existed:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(backup / name, target)
        else:
            target.unlink(missing_ok=True)


def install(bundle, root):
    root = root.resolve()
    source = bundle / "project"
    bundled_cloud = source / ".env.cloud"
    if not (root / ".env.cloud").is_file() and not bundled_cloud.is_file():
        raise RuntimeError(
            "Missing .env.cloud. Configure Supabase first; no keys/source changed."
        )
    changes = {}
    for path in source.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(source)
        if (
            any(
                part in ("data", "backups", "__pycache__", ".venv", "node_modules")
                for part in rel.parts
            )
            or path.suffix == ".pyc"
        ):
            continue
        if path.name in (".env", ".env.cloud", "secrets.h"):
            continue
        target = (root / rel).resolve()
        if not target.is_relative_to(root):
            raise ValueError("Invalid source path")
        preserve = (
            rel.parts[0] in ("models", "recordings")
            or path.name == "config.h"
            or rel.as_posix() == "config/gestures.json"
        )
        if preserve and target.exists():
            continue
        content = path.read_bytes()
        if path.suffix == ".py":
            ast.parse(content.decode("utf-8-sig"), filename=str(rel))
        if not target.exists() or target.read_bytes() != content:
            changes[target] = content
    env = root / ".env"
    text = (
        env.read_text(encoding="utf-8")
        if env.exists()
        else (source / ".env.example").read_text(encoding="utf-8")
    )
    values = dotenv_values(stream=StringIO(text))
    new_key = not values.get("API_KEY")
    if new_key:
        value = "API_KEY=" + secrets.token_urlsafe(32)
        text, count = re.subn(r"(?m)^\s*API_KEY\s*=.*$", value, text)
        if not count:
            text = text.rstrip() + "\n" + value + "\n"
    text, count = re.subn(r"(?m)^\s*CLOUD_ENABLED\s*=.*$", "CLOUD_ENABLED=1", text)
    if not count:
        text = text.rstrip() + "\nCLOUD_ENABLED=1\n"
    if not env.exists() or env.read_text(encoding="utf-8") != text:
        changes[env] = text.encode("utf-8")
    if not (root / ".env.cloud").exists() and bundled_cloud.is_file():
        changes[root / ".env.cloud"] = bundled_cloud.read_bytes()
    if (bundle / "README.md").exists():
        changes[root / "README.md"] = (bundle / "README.md").read_bytes()
    backup = (
        root / "backups" / ("cloud-v6-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    )
    backup.mkdir(parents=True, mode=0o700)
    manifest = {path.relative_to(root).as_posix(): path.exists() for path in changes}
    for path in changes:
        if path.exists():
            target = backup / path.relative_to(root)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            if path.name == ".env":
                target.chmod(0o600)
    (backup / "manifest.json").write_text(json.dumps(manifest, indent=2))
    try:
        for path, content in changes.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        env.chmod(0o600)
        (root / ".env.cloud").chmod(0o600)
    except BaseException:
        restore(root, backup)
        raise
    print("Updated", len(changes), "files. Backup:", backup)
    print(
        "Existing Supabase/device credentials preserved. Dashboard API_KEY "
        + ("created because it was empty." if new_key else "preserved.")
    )
    print(
        "View dashboard key privately: ~/ai-camera/.venv/bin/python ~/ai-camera-release/scripts/dashboard_key.py --show"
    )
    return backup


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--target", type=Path, default=Path.home() / "ai-camera-release"
    )
    parser.add_argument("--rollback", type=Path)
    args = parser.parse_args()
    if args.rollback:
        restore(args.target.resolve(), args.rollback.resolve())
        print("Restored. Restart ai-camera.")
    else:
        install(Path(__file__).resolve().parent, args.target)
