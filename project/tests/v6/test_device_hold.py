import tempfile
import unittest
from types import SimpleNamespace

from app.cloud_control import AUTO_HOLD_SECONDS, DeviceControl, device_hold
from app.local_jobs import LocalJobs


class DeviceHold(unittest.TestCase):
    """hold_required: so giay giu tay ma web hien thi cho ESP32."""

    def test_device_value_preferred_and_validated(self):
        self.assertEqual(device_hold({"hold_required": 3.0}), 3.0)
        self.assertEqual(device_hold({}), AUTO_HOLD_SECONDS)
        for bad in (0, -1, 99, "abc", None, [1]):
            self.assertEqual(device_hold({"hold_required": bad}), AUTO_HOLD_SECONDS)

    def test_snapshot_reports_device_hold(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = SimpleNamespace(jobs=LocalJobs(directory), client=None)
            control = DeviceControl(runtime)
            snap = control.snapshot("esp32-01")
            self.assertEqual(snap["hold_required"], AUTO_HOLD_SECONDS)
            self.assertFalse(snap["online"])

            control.report(
                "esp32-01", {"mode": "auto", "running": True, "hold": 1.5, "hold_required": 4}
            )
            snap = control.snapshot("esp32-01")
            self.assertEqual(snap["hold_required"], 4)
            self.assertEqual(snap["hold_seconds"], 1.5)
            self.assertEqual(snap["mode"], "auto")
            self.assertTrue(snap["online"])


if __name__ == "__main__":
    unittest.main()
