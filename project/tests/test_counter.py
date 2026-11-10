import math
import unittest
from app.finger_counter import choose_hands, count_hand, summarize


def hand(number):
    pts = [{"x": 0.5, "y": 0.93} for _ in range(21)]
    extended = set(range(1, number + 1)) if number < 5 else set(range(5))
    if number == 0:
        extended = set()
    if number == 1:
        extended = {0}
    # 2/3/4: trỏ + giữa + áp út + út, ngón cái khép.
    if number in (2, 3, 4):
        extended = set(range(1, number + 1))
    pts[1] = {"x": 0.4, "y": 0.74}
    pts[2] = {"x": 0.28, "y": 0.67}
    if 0 in extended:
        pts[3] = {"x": 0.16, "y": 0.60}
        pts[4] = {"x": 0.04, "y": 0.53}
    else:
        pts[3] = {"x": 0.40, "y": 0.64}
        pts[4] = {"x": 0.51, "y": 0.66}
    for fi, (mcp, x, y) in enumerate(
        [(5, 0.4, 0.58), (9, 0.52, 0.55), (13, 0.64, 0.58), (17, 0.76, 0.64)], 1
    ):
        pts[mcp] = {"x": x, "y": y}
        positions = (
            [y - 0.18, y - 0.30, y - 0.40] if fi in extended else [y - 0.16, y - 0.03, y + 0.06]
        )
        for offset, fy in enumerate(positions, 1):
            pts[mcp + offset] = {"x": x, "y": fy}
    return pts


class Counts(unittest.TestCase):
    def test_zero_to_five_and_geometry_transforms(self):
        for n in range(6):
            for degrees in (0, 45, 90, 180, 270):
                for mirror in (1, -1):
                    theta = math.radians(degrees)
                    pts = []
                    for p in hand(n):
                        x = (p["x"] - 0.5) * mirror * 0.75
                        y = (p["y"] - 0.5) * 0.75
                        pts.append(
                            {
                                "x": 0.5 + x * math.cos(theta) - y * math.sin(theta),
                                "y": 0.5 + x * math.sin(theta) + y * math.cos(theta),
                            }
                        )
                    self.assertEqual(count_hand(pts, 640, 640)["count"], n, (n, degrees, mirror))

    def test_total_one_to_ten(self):
        for n in range(1, 11):
            counts = [min(n, 5)] if n <= 5 else [5, n - 5]
            hands = [{"finger_count": count_hand(hand(c), 640, 640)["count"]} for c in counts]
            result = summarize(hands)
            self.assertEqual(result["finger_count"], n)
            self.assertEqual(result["gesture"], f"Count_{n}")
            self.assertIsNone(result["score"])

    def test_invalid_or_ambiguous_is_not_guessed(self):
        self.assertIsNone(count_hand([], 640, 640)["count"])
        self.assertIsNone(count_hand([{"x": 0.5, "y": 0.5}] * 21, 640, 640)["count"])
        self.assertEqual(summarize([])["gesture"], "No_hand")
        self.assertEqual(
            summarize([{"finger_count": None}, {"finger_count": 5}])["gesture"], "Unknown"
        )
        self.assertEqual(summarize([{"finger_count": 0}])["gesture"], "Unknown")

    def test_choose_hands_spread_pair_vs_two_people(self):
        def box(cx, cy, side, size=0.3):
            pts = [
                {"x": cx - size / 2, "y": cy - size / 2},
                {"x": cx + size / 2, "y": cy + size / 2},
            ]
            return {"handedness": side, "landmarks": pts}

        # Mot nguoi gio hai tay dang rong (anh 4 + 4 ngon): giu ca hai.
        spread = [box(0.26, 0.5, "Right"), box(0.74, 0.5, "Left")]
        self.assertEqual(len(choose_hands(spread)), 2)
        # Hai tay cung phia (thuong la hai nguoi): chi lay tay lon nhat.
        same_side = [box(0.26, 0.5, "Right"), box(0.74, 0.5, "Right")]
        self.assertEqual(len(choose_hands(same_side)), 1)
        # Lech doc qua nhieu: khong cong.
        uneven = [box(0.26, 0.2, "Right"), box(0.74, 0.8, "Left")]
        self.assertEqual(len(choose_hands(uneven)), 1)


if __name__ == "__main__":
    unittest.main()
