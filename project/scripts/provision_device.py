"""Register another device. Run on a trusted machine with the server-only .env.cloud."""

import argparse, json, os, re, secrets, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app.supabase_client import SupabaseClient


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="esp32-01")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", args.device):
        raise SystemExit("Invalid device ID")
    client = SupabaseClient()
    out = args.output
    out.mkdir(parents=True, exist_ok=True, mode=0o700)
    saved = out / "device-account.json"
    email = args.device.lower() + "@devices.ai-camera.invalid"
    if saved.exists():
        account = json.loads(saved.read_text())
    else:
        for page in range(1, 101):
            users = client.request("GET", f"/auth/v1/admin/users?page={page}&per_page=1000").get(
                "users", []
            )
            if any(u.get("email") == email for u in users):
                raise SystemExit(
                    "Account exists; recover its private configuration before proceeding"
                )
            if len(users) < 1000:
                break
        password = secrets.token_urlsafe(32)
        user = client.request(
            "POST",
            "/auth/v1/admin/users",
            {
                "email": email,
                "password": password,
                "email_confirm": True,
                "user_metadata": {"device_id": args.device},
            },
        )
        account = {"email": email, "password": password, "uid": user["id"]}
        saved.write_text(json.dumps(account))
        saved.chmod(0o600)
    rows = client.request("GET", "/rest/v1/ai_devices?id=eq." + args.device + "&select=user_id")
    if rows and rows[0]["user_id"] != account["uid"]:
        raise SystemExit("Device ID belongs to another identity")
    if not rows:
        client.request(
            "POST", "/rest/v1/ai_devices", {"id": args.device, "user_id": account["uid"]}
        )
    publishable = os.environ.get("SUPABASE_PUBLISHABLE_KEY", "")
    if not publishable:
        raise SystemExit("Set SUPABASE_PUBLISHABLE_KEY first; account file has been saved")
    local = os.environ.get("HYBRID_API_KEY") or secrets.token_urlsafe(32)
    values = {
        "SUPABASE_URL": client.url,
        "SUPABASE_PUBLISHABLE_KEY": publishable,
        "DEVICE_ID": args.device,
        "DEVICE_UID": account["uid"],
        "DEVICE_EMAIL": account["email"],
        "DEVICE_PASSWORD": account["password"],
    }
    (out / "device.env").write_text("\n".join(k + "=" + v for k, v in values.items()) + "\n")
    (out / "device.env").chmod(0o600)
    header = "#pragma once\n" + "".join(
        "static constexpr char " + k + "[] = " + json.dumps(v) + ";\n"
        for k, v in {
            "SB_URL": client.url,
            "SB_KEY": publishable,
            "SB_EMAIL": account["email"],
            "SB_PASSWORD": account["password"],
            "SB_UID": account["uid"],
            "HYBRID_KEY": local,
        }.items()
    )
    (out / "secrets.h").write_text(header)
    (out / "secrets.h").chmod(0o600)
    (out / "pi-device.env").write_text(
        f"CLOUD_DEVICE_ID={args.device}\nCLOUD_DEVICE_UID={account['uid']}\nHYBRID_API_KEY={local}\n"
    )
    (out / "pi-device.env").chmod(0o600)
    print("Registered device. Private files saved to:", out.resolve())
    print("No passwords or API keys printed. Merge pi-device.env into the Pi .env.cloud.")


if __name__ == "__main__":
    main()
