"""Đếm ngón bằng landmark MediaPipe; không dùng nhãn 7 cử chỉ có sẵn."""

import math

FINGER_NAMES = ("thumb", "index", "middle", "ring", "pinky")


def count_hand(landmarks, width, height):
    unknown = {
        "count": None,
        "fingers": {name: "uncertain" for name in FINGER_NAMES},
        "reason": "Tay chưa đủ rõ",
    }
    if len(landmarks) != 21 or width <= 0 or height <= 0:
        return unknown

    def get(p, k):
        return p[k] if isinstance(p, dict) else getattr(p, k)

    try:
        normalized = [(float(get(p, "x")), float(get(p, "y"))) for p in landmarks]
    except (TypeError, KeyError, AttributeError, ValueError):
        return unknown
    if not all(math.isfinite(v) and -0.03 <= v <= 1.03 for pt in normalized for v in pt):
        return unknown
    p = [(x * width, y * height) for x, y in normalized]

    def d(a, b):
        return math.dist(p[a], p[b])

    def angle(a, b, c):
        u = (p[a][0] - p[b][0], p[a][1] - p[b][1])
        v = (p[c][0] - p[b][0], p[c][1] - p[b][1])
        norm = math.hypot(*u) * math.hypot(*v)
        if norm < 1e-6:
            return None
        return math.degrees(math.acos(max(-1, min(1, (u[0] * v[0] + u[1] * v[1]) / norm))))

    palm = d(5, 17)
    if palm < 18 or d(0, 9) < 0.45 * palm:
        return unknown
    states = {}
    for name, mcp in zip(FINGER_NAMES[1:], (5, 9, 13, 17)):
        pip, dip, tip = mcp + 1, mcp + 2, mcp + 3
        a, b = angle(mcp, pip, dip), angle(pip, dip, tip)
        length = d(mcp, pip) + d(pip, dip) + d(dip, tip)
        if a is None or b is None or min(d(mcp, pip), d(pip, dip), d(dip, tip)) < 0.025 * palm:
            state = "uncertain"
        elif a > 155 and b > 140 and d(mcp, tip) > 0.86 * length and d(0, tip) > 1.08 * d(0, pip):
            state = "extended"
        elif d(0, tip) < 0.98 * d(0, pip) or (a < 125 and d(mcp, tip) < 0.85 * length):
            state = "folded"
        else:
            state = "uncertain"
        states[name] = state
    a, b = angle(1, 2, 3), angle(2, 3, 4)
    if a is None or b is None or min(d(1, 2), d(2, 3), d(3, 4)) < 0.025 * palm:
        thumb = "uncertain"
    elif (
        a > 150
        and b > 150
        and d(4, 5) > 0.75 * palm
        and d(4, 17) > 1.3 * palm
        and d(4, 17) > d(3, 17) + 0.12 * palm
    ):
        thumb = "extended"
    elif d(4, 5) < 0.65 * palm or d(4, 17) < 1.15 * palm or a < 130:
        thumb = "folded"
    else:
        thumb = "uncertain"
    states = {"thumb": thumb, **states}
    count = (
        None if "uncertain" in states.values() else sum(s == "extended" for s in states.values())
    )
    visible = sum(s == "extended" for s in states.values())
    return {
        "count": count,
        "fingers": states,
        "reason": None if count is not None else "Có ngón bị khuất hoặc chưa duỗi/gập rõ",
        "visible_count": visible,
        "partial": count is None and visible > 0,
    }


def summarize(hands):
    """Chỉ trả 1–10 nếu cả hai bàn tay phát hiện được đều đếm đủ rõ."""
    if not hands:
        return {
            "gesture": "No_hand",
            "score": None,
            "method": "landmark_count",
            "finger_count": None,
        }
    if any(h.get("finger_count") is None for h in hands):
        return {
            "gesture": "Unknown",
            "score": None,
            "method": "landmark_count",
            "finger_count": None,
            "partial": any(h.get("partial") for h in hands),
            "visible_finger_count": sum(h.get("visible_count", 0) for h in hands),
        }
    total = sum(h["finger_count"] for h in hands)
    if not 1 <= total <= 10:
        return {
            "gesture": "Unknown",
            "score": None,
            "method": "landmark_count",
            "finger_count": None,
        }
    return {
        "gesture": f"Count_{total}",
        "score": None,
        "method": "landmark_count",
        "finger_count": total,
        "partial": False,
        "visible_finger_count": total,
    }


def choose_hands(hands):
    """Trong ảnh nhiều người, chỉ cộng hai tay nếu chúng gần và cùng cỡ."""
    if len(hands) < 2:
        return hands

    def box(hand):
        points = hand.get("landmarks") or []
        if not points:
            return (0, 0, 0)
        xs = [p["x"] for p in points]
        ys = [p["y"] for p in points]
        return (
            (max(xs) - min(xs)) * (max(ys) - min(ys)),
            (min(xs) + max(xs)) / 2,
            (min(ys) + max(ys)) / 2,
        )

    ranked = sorted(hands, key=lambda h: box(h)[0], reverse=True)
    a, ax, ay = box(ranked[0])
    b, bx, by = box(ranked[1])
    if b <= 0 or a / b >= 1.8:
        return ranked[:1]
    distance = math.dist((ax, ay), (bx, by))
    if distance <= 0.42:
        return ranked[:2]
    # Một người giơ hai tay dang rộng: một tay trái + một tay phải, cùng cỡ, ngang nhau.
    pair = {ranked[0].get("handedness"), ranked[1].get("handedness")} == {"Left", "Right"}
    if pair and distance <= 0.7 and abs(ay - by) <= 0.25:
        return ranked[:2]
    return ranked[:1]
