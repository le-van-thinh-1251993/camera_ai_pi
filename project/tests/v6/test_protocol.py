import unittest
from app.cloud_protocol import auto_decision, validate_input, is_permanent


class Protocol(unittest.TestCase):
    rid = "00000000-0000-4000-8000-000000000001"
    uid = "00000000-0000-4000-8000-000000000002"

    def frames(self):
        return [
            {"path": f"{self.uid}/{self.rid}.jpg", "ms": 0},
            {"path": f"{self.uid}/{self.rid}/1.jpg", "ms": 1000},
            {"path": f"{self.uid}/{self.rid}/2.jpg", "ms": 2000},
        ]

    def test_capture_time_independent_from_network(self):
        frames = self.frames()
        validate_input(self.rid, self.uid, frames[0]["path"], {"mode": "auto", "frames": frames})
        events = [{"finger_count": 2} for _ in frames]
        self.assertTrue(auto_decision(events, frames))
        events[1]["finger_count"] = None
        self.assertTrue(auto_decision(events, frames))
        events[1]["finger_count"] = 1
        self.assertFalse(auto_decision(events, frames))
        events[1]["finger_count"] = 2
        events[0]["finger_count"] = None
        self.assertFalse(auto_decision(events, frames))

    def test_single_auto_transport_accepts_one_frame(self):
        frame = [{"path": f"{self.uid}/{self.rid}.jpg", "ms": 0}]
        result = validate_input(
            self.rid,
            self.uid,
            frame[0]["path"],
            {"mode": "manual", "capture_mode": "auto", "frames": frame},
        )
        self.assertEqual(result, frame)

    def test_short_hold_and_cross_owner_frames_rejected(self):
        frames = self.frames()
        frames[-1]["ms"] = 1999
        with self.assertRaises(ValueError):
            validate_input(
                self.rid, self.uid, frames[0]["path"], {"mode": "auto", "frames": frames}
            )
        frames = self.frames()
        frames[-1]["path"] = "other-owner/private.jpg"
        with self.assertRaises(ValueError):
            validate_input(
                self.rid, self.uid, frames[0]["path"], {"mode": "auto", "frames": frames}
            )

    def test_bad_image_permanent_busy_temporary(self):
        class Error(Exception):
            def __init__(self, status):
                self.status_code = status

        self.assertTrue(is_permanent(Error(422)))
        self.assertFalse(is_permanent(Error(429)))
        self.assertFalse(is_permanent(Error(503)))


if __name__ == "__main__":
    unittest.main()
