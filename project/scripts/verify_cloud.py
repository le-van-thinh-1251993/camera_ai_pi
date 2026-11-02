"""Exercise the installed Pi through its real Cloud HTTP routes; no hardware is simulated as verified."""

import argparse, json, time
from pathlib import Path
import httpx
from dotenv import dotenv_values


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8000")
    args = parser.parse_args()
    key = dotenv_values(root / ".env").get("API_KEY")
    if not key:
        raise SystemExit("Thiếu API_KEY dashboard trong .env")
    with httpx.Client(base_url=args.url, headers={"X-API-Key": key}, timeout=30) as client:
        assert client.get("/api/history", headers={"X-API-Key": ""}).status_code == 401
        public_health = client.get("/health").json()
        assert "mediapipe" not in public_health and "error" not in public_health
        report = client.get("/api/diagnostics").json()

        def submit(camera=False, automatic=False, broken=False):
            sample = (
                (root / "samples/victory.jpg").read_bytes() if not broken else b"\xff\xd8broken"
            )
            files = [
                ("files" if camera else "file", (str(i) + ".jpg", sample, "image/jpeg"))
                for i in range(1)
            ]
            response = client.post(
                "/api/camera" if camera else "/api/upload",
                files=files,
                data={
                    "mode": "manual" if camera else "upload",
                    "capture_mode": "auto" if automatic else "manual",
                    "captures_ms": "[0]",
                },
            )
            response.raise_for_status()
            job = response.json()
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                result = client.get(job["status_url"])
                result.raise_for_status()
                body = result.json()
                if body["status"] in ("completed", "acked", "failed"):
                    return body
                time.sleep(1)
            raise RuntimeError("Chờ Cloud quá lâu")

        photo = submit()
        assert photo["result"]["cloud"] and photo["result"]["finger_count"] == 2
        auto = submit(camera=True, automatic=True)
        assert (
            auto["result"]["confirmed"]
            and auto["result"]["stable"]
            and auto["result"]["frames_checked"] == 1
        )
        manual = submit(camera=True)
        assert manual["result"]["cloud"] and manual["result"]["confirmed"]
        assert client.get(photo["result"]["audio_url"]).content.startswith(b"RIFF")
        bad = submit(broken=True)
        assert bad["status"] == "failed"
        assert client.post("/api/frame", content=b"bad").status_code == 410
        report.update(
            upload_cloud=True,
            camera_auto_cloud=True,
            camera_manual_cloud=True,
            auto_hold_ms=auto["result"]["hold_ms"],
            wav_valid=True,
            broken_image_failed=True,
            legacy_inference_disabled=True,
        )
        output = root / "data/validation"
        output.mkdir(parents=True, exist_ok=True)
        (output / "verify-cloud.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        print(json.dumps(report, ensure_ascii=False, indent=2))
        print("Camera ở bài thử này dùng ảnh mẫu mô phỏng; cần thử thêm iPhone/ESP thật.")


if __name__ == "__main__":
    main()
