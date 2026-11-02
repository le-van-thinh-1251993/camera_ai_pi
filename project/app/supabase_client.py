"""Bounded HTTPS REST client. Secret API keys are sent only in apikey headers."""

import json
import os
import time
from email.utils import parsedate_to_datetime
from pathlib import Path
from urllib.parse import quote, urlsplit
from urllib.error import HTTPError, URLError
import httpx
from dotenv import load_dotenv

ROOT = Path(__file__).resolve().parents[1]
# Allow standalone scripts and systemd launches to use the same project-local
# configuration as the main application.  Existing values are preserved.
load_dotenv(ROOT / ".env", override=False)
load_dotenv(ROOT / ".env.cloud", override=False)


class CloudError(RuntimeError):
    def __init__(self, status=0, code="network_error"):
        self.status = status
        self.code = code
        super().__init__(f"Supabase HTTP {status}: {code}")


class SupabaseClient:
    def __init__(self, url=None, key=None, token=None, timeout=15):
        self.url = (url or os.getenv("SUPABASE_URL", "")).rstrip("/")
        # Older Pi installs used SERVICE_ROLE_KEY or SUPABASE_KEY.  Accept
        # those aliases, while preferring the current secret-key name.
        self.key = key or next(
            (
                os.getenv(name, "").strip()
                for name in (
                    "SUPABASE_SECRET_KEY",
                    "SUPABASE_SERVICE_ROLE_KEY",
                    "SUPABASE_KEY",
                )
                if os.getenv(name, "").strip()
            ),
            "",
        )
        parts = urlsplit(self.url)
        if (
            parts.scheme != "https"
            or not parts.hostname
            or not parts.hostname.endswith(".supabase.co")
            or parts.username
            or parts.path not in ("", "/")
        ):
            raise ValueError("SUPABASE_URL phải là HTTPS project Supabase")
        if not self.key:
            raise ValueError("Thiếu khóa Supabase")
        self.token = token
        self.timeout = timeout
        self.clock_offset = 0
        self.http = httpx.Client(
            timeout=httpx.Timeout(
                timeout,
                connect=min(5, timeout),
                pool=min(5, timeout),
            ),
            follow_redirects=False,
            limits=httpx.Limits(
                max_connections=8, max_keepalive_connections=4, keepalive_expiry=30
            ),
        )

    def request(
        self,
        method,
        path,
        data=None,
        content_type="application/json",
        limit=2 * 1024 * 1024,
        headers=None,
        raw=False,
    ):
        if not path.startswith("/") or path.startswith("//"):
            raise ValueError("Invalid API path")
        body = (
            data
            if isinstance(data, bytes) or data is None
            else json.dumps(data, ensure_ascii=False).encode("utf-8")
        )
        h = {"apikey": self.key, "Content-Type": content_type}
        if self.token:
            h["Authorization"] = "Bearer " + self.token
        h.update(headers or {})
        try:
            with self.http.stream(method, self.url + path, content=body, headers=h) as response:
                if response.headers.get("Date"):
                    self.clock_offset = (
                        parsedate_to_datetime(response.headers["Date"]).timestamp() - time.time()
                    )
                value = bytearray()
                for chunk in response.iter_bytes():
                    value.extend(chunk)
                    if len(value) > limit:
                        raise CloudError(413, "response_too_large")
                value = bytes(value)
                if response.status_code >= 400:
                    try:
                        error = json.loads(value)
                        code = str(error.get("code") or error.get("error") or "request_rejected")[
                            :80
                        ]
                    except Exception:
                        code = "request_rejected"
                    raise CloudError(response.status_code, code)
                if raw:
                    return value
                return json.loads(value) if value else None
        except HTTPError as exc:
            try:
                error = json.loads(exc.read(4096))
                code = str(error.get("code") or error.get("error") or "request_rejected")[:80]
            except Exception:
                code = "request_rejected"
            raise CloudError(exc.code, code) from None
        except (httpx.HTTPError, URLError, TimeoutError, OSError) as exc:
            # Keep a safe, short class-only diagnostic.  Never include URLs,
            # headers, request bodies, or credentials in logs/API responses.
            kind = type(exc).__name__
            raise CloudError(0, "network_" + kind[:48]) from exc

    def close(self):
        self.http.close()

    def rpc(self, name, args):
        return self.request("POST", "/rest/v1/rpc/" + quote(name, safe=""), args)

    def get_job(self, request_id):
        rows = self.request(
            "GET",
            "/rest/v1/ai_requests?id=eq." + quote(str(request_id), safe="") + "&select=*&limit=1",
        )
        return rows[0] if rows else None

    def download(self, bucket, path, limit=15 * 1024 * 1024):
        return self.request(
            "GET",
            "/storage/v1/object/authenticated/"
            + quote(bucket, safe="")
            + "/"
            + quote(path, safe="/"),
            limit=limit,
            raw=True,
        )

    def upload(self, bucket, path, data, mime):
        return self.request(
            "POST",
            "/storage/v1/object/" + quote(bucket, safe="") + "/" + quote(path, safe="/"),
            data,
            mime,
            headers={"x-upsert": "true"},
        )

    def login(self, email, password):
        result = self.request(
            "POST", "/auth/v1/token?grant_type=password", {"email": email, "password": password}
        )
        self.token = result["access_token"]
        return result
