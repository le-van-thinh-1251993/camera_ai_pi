"""Thống kê cho trang Biểu đồ: tỉ lệ nhận diện, kết quả theo nguồn, thời gian xử lý."""

from datetime import datetime, timedelta

RANGES = ("today", "7d", "all")
OUTCOMES = ("ok", "unknown", "no_hand")
SOURCES = ("esp32", "browser", "upload")
LATENCY_POINTS = 60


def outcome(event):
    if event.get("finger_count") or str(event.get("gesture", "")).startswith("Count_"):
        return "ok"
    if event.get("gesture") == "No_hand":
        return "no_hand"
    return "unknown"


def source(event):
    value = event.get("source")
    return value if value in ("browser", "upload") else "esp32"


def since(range_name, now):
    """Mốc bắt đầu theo giờ địa phương của Pi; None = tất cả."""
    if range_name == "today":
        return now.replace(hour=0, minute=0, second=0, microsecond=0)
    if range_name == "7d":
        return now - timedelta(days=7)
    return None


def _time(event):
    try:
        return datetime.fromisoformat(str(event.get("time"))).astimezone()
    except ValueError:
        return None


def compute_stats(events, range_name="today", now=None):
    """events: payload lịch sử theo thứ tự cũ -> mới."""
    if range_name not in RANGES:
        raise ValueError("range phải là today, 7d hoặc all")
    now = (now or datetime.now()).astimezone()
    start = since(range_name, now)
    totals = dict.fromkeys(OUTCOMES, 0)
    by_source = {name: dict.fromkeys(OUTCOMES, 0) for name in SOURCES}
    latency = []
    for event in events:
        if event.get("pipeline_version") == 6 and not event.get("confirmed"):
            continue
        when = _time(event)
        if start is not None and (when is None or when < start):
            continue
        kind, src = outcome(event), source(event)
        totals[kind] += 1
        by_source[src][kind] += 1
        if event.get("latency_ms") is not None and when is not None:
            latency.append(
                {
                    "time": when.isoformat(),
                    "ms": int(event["latency_ms"]),
                    "source": src,
                    "outcome": kind,
                }
            )
    total = sum(totals.values())
    return {
        "range": range_name,
        "total": total,
        "success_rate": round(totals["ok"] / total, 4) if total else None,
        "outcomes": totals,
        "by_source": by_source,
        "latency": latency[-LATENCY_POINTS:],
    }
