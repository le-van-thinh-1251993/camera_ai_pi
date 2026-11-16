import importlib.util
import unittest
from datetime import datetime, timezone
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "pi_lcd.py"
spec = importlib.util.spec_from_file_location("pi_lcd", SCRIPT)
pi_lcd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pi_lcd)


def info(**state):
    return {
        "ip": "192.168.1.20",
        "wifi": "Home",
        "host": "Pi-AI",
        "web": True,
        "cloud": False,
        "state": state,
    }


class PiLcdRender(unittest.TestCase):
    """Ve bang trang thai khong can man that (giong --png), ca khi may khong co font DejaVu."""

    def test_render_sizes_and_rotation(self):
        for size in [(240, 240), (240, 320), (320, 240), (135, 240)]:
            img = pi_lcd.render(info(), size)
            self.assertEqual(img.size, size)
            self.assertEqual(img.mode, "RGB")
            self.assertEqual(img.getpixel((size[0] - 1, size[1] - 1)), pi_lcd.BG)

    def test_render_online_device_with_result(self):
        now = datetime.now(timezone.utc).isoformat()
        img = pi_lcd.render(
            info(online=True, mode="auto", result={"finger_count": 3, "time": now}), (240, 240)
        )
        colors = {c for _, c in img.getcolors(maxcolors=1 << 16)}
        self.assertIn(pi_lcd.PANEL, colors)  # the ket qua gan nhat duoc ve

    def test_render_without_server_state(self):
        data = info()
        data["state"] = None
        data["web"] = False
        self.assertEqual(pi_lcd.render(data, (240, 280)).size, (240, 280))

    def test_test_pattern(self):
        img = pi_lcd.test_pattern((240, 240))
        self.assertEqual(img.getpixel((10, 10)), (255, 0, 0))
        self.assertEqual(img.getpixel((0, 239)), (255, 255, 0))  # khung vang

    def test_gesture_text_and_ago(self):
        self.assertEqual(pi_lcd.gesture_text(None)[0], "Chưa có")
        self.assertEqual(pi_lcd.gesture_text({"finger_count": 7})[0], "7 ngón")
        self.assertEqual(pi_lcd.gesture_text({"gesture": "No_hand"})[0], "Không thấy tay")
        self.assertEqual(pi_lcd.ago("not-a-date"), "")
        self.assertTrue(pi_lcd.ago(datetime.now(timezone.utc).isoformat()).endswith("giây trước"))


if __name__ == "__main__":
    unittest.main()
