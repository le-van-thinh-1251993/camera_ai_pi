import unittest
from datetime import datetime, timedelta, timezone

from app.stats import compute_stats

NOW = datetime(2026, 11, 10, 15, 0, tzinfo=timezone.utc)


def event(gesture, source="upload", hours_ago=1, latency=100, **extra):
    return {
        "gesture": gesture,
        "finger_count": int(gesture.split("_")[1]) if gesture.startswith("Count_") else None,
        "source": source,
        "time": (NOW - timedelta(hours=hours_ago)).isoformat(),
        "latency_ms": latency,
        "confirmed": True,
        "pipeline_version": 6,
        **extra,
    }


class StatsTest(unittest.TestCase):
    def test_outcomes_sources_and_rate(self):
        events = [
            event("Count_2", "esp32"),
            event("Count_8", "upload"),
            event("Unknown", "browser"),
            event("No_hand", "cloud"),
            event("Count_3", "esp32", confirmed=False),  # chua xac nhan: bo qua
        ]
        result = compute_stats(events, "all", NOW)
        self.assertEqual(result["total"], 4)
        self.assertEqual(result["outcomes"], {"ok": 2, "unknown": 1, "no_hand": 1})
        self.assertEqual(result["success_rate"], 0.5)
        self.assertEqual(result["by_source"]["esp32"], {"ok": 1, "unknown": 0, "no_hand": 1})
        self.assertEqual(result["by_source"]["browser"]["unknown"], 1)
        self.assertEqual([p["ms"] for p in result["latency"]], [100, 100, 100, 100])

    def test_ranges(self):
        events = [event("Count_1", hours_ago=24 * 10), event("Count_2", hours_ago=24 * 3)]
        self.assertEqual(compute_stats(events, "all", NOW)["total"], 2)
        self.assertEqual(compute_stats(events, "7d", NOW)["total"], 1)
        empty = compute_stats([], "today", NOW)
        self.assertEqual(empty["total"], 0)
        self.assertIsNone(empty["success_rate"])
        with self.assertRaises(ValueError):
            compute_stats(events, "year", NOW)


if __name__ == "__main__":
    unittest.main()
