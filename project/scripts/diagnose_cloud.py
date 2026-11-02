"""Safe Supabase connectivity check for a Pi installation."""

import os
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit

from dotenv import load_dotenv

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
load_dotenv(ROOT / ".env", override=False)
load_dotenv(ROOT / ".env.cloud", override=False)

from app.supabase_client import CloudError, SupabaseClient


def check(client, method, path, data=None):
    started = time.monotonic()
    try:
        client.request(method, path, data)
        print("OK", method, path.split("?")[0], round(time.monotonic() - started, 2), "s")
    except CloudError as exc:
        print("FAIL", method, path.split("?")[0], "HTTP", exc.status, "code", exc.code)


def main():
    client = SupabaseClient(timeout=12)
    try:
        print("Host:", urlsplit(client.url).hostname)
        print("Có khóa:", bool(client.key))
        check(client, "GET", "/rest/v1/ai_devices?select=id&limit=1")
        check(
            client,
            "POST",
            "/rest/v1/rpc/ai_claim_v6",
            {"p_worker": os.getenv("CLOUD_WORKER_ID", "diagnostic"), "p_seconds": 30},
        )
    finally:
        client.close()


if __name__ == "__main__":
    main()
