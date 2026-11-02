"""MediaPipe image inference. Network requests never call this outside the cloud worker."""

import logging, os, threading
from pathlib import Path
from io import BytesIO
import cv2
import mediapipe as mp
import numpy as np
from PIL import Image
from app.finger_counter import count_hand, summarize, choose_hands


def setting(name, default):
    return os.getenv(name, str(default))


MODEL = Path(
    setting("MODEL_PATH", Path(__file__).resolve().parents[1] / "models/gesture_recognizer.task")
)
PHOTO_PIXELS = int(setting("MAX_PHOTO_PIXELS", 50000000))
log = logging.getLogger("ai-camera.vision")


class Engine:
    def __init__(self):
        self.lock = threading.Lock()
        self.rec = None
        self.error = None
        try:
            self.rec = mp.tasks.vision.GestureRecognizer.create_from_options(
                mp.tasks.vision.GestureRecognizerOptions(
                    base_options=mp.tasks.BaseOptions(model_asset_path=str(MODEL)),
                    running_mode=mp.tasks.vision.RunningMode.IMAGE,
                    num_hands=2,
                )
            )
        except Exception as exc:
            self.error = str(exc)
            log.exception("Không khởi tạo được MediaPipe")

    def _scan(self, arr, box):
        h, w = arr.shape[:2]
        x0, y0, x1, y1 = box
        crop = arr[y0:y1, x0:x1]
        scale = min(1, int(setting("MAX_SIDE", 1280)) / max(crop.shape[:2]))
        if scale < 1:
            crop = cv2.resize(
                crop, (max(1, round(crop.shape[1] * scale)), max(1, round(crop.shape[0] * scale)))
            )
        with self.lock:
            result = self.rec.recognize(
                mp.Image(
                    image_format=mp.ImageFormat.SRGB, data=cv2.cvtColor(crop, cv2.COLOR_BGR2RGB)
                )
            )
        hands = []
        for i, points in enumerate(result.hand_landmarks):
            count = count_hand(points, crop.shape[1], crop.shape[0])
            label = (
                result.handedness[i][0].category_name
                if i < len(result.handedness) and result.handedness[i]
                else "Unknown"
            )
            landmarks = [
                {
                    "x": float((x0 + p.x * (x1 - x0)) / w),
                    "y": float((y0 + p.y * (y1 - y0)) / h),
                    "z": float(p.z),
                }
                for p in points
            ]
            hands.append(
                {
                    "finger_count": count["count"],
                    "fingers": count["fingers"],
                    "reason": count["reason"],
                    "visible_count": count.get("visible_count", 0),
                    "partial": count.get("partial", False),
                    "handedness": label,
                    "landmarks": landmarks,
                }
            )
        return hands

    def run(self, raw, source="browser", focus=None, all_hands=False):
        photo = source in ("upload", "cloud")
        select = (lambda hands: hands) if all_hands else choose_hands
        max_pixels = PHOTO_PIXELS if photo else int(setting("MAX_PIXELS", 12000000))
        if photo:
            try:
                with Image.open(BytesIO(raw)) as metadata:
                    if metadata.width * metadata.height > max_pixels:
                        raise ValueError("Ảnh vượt giới hạn 50 triệu điểm ảnh")
            except ValueError:
                raise
            except Exception as exc:
                raise ValueError("Không đọc được kích thước ảnh JPEG/PNG") from exc
        arr = cv2.imdecode(np.frombuffer(raw, dtype=np.uint8), cv2.IMREAD_COLOR)
        if arr is None:
            raise ValueError("Không giải mã được ảnh JPEG/PNG")
        h, w = arr.shape[:2]
        if h * w > max_pixels:
            raise ValueError("Ảnh vượt giới hạn số điểm ảnh")
        if focus is not None:
            fx, fy = focus
            span = 0.48
            x0 = max(0, min(1 - span, fx - span / 2))
            y0 = max(0, min(1 - span, fy - span / 2))
            box = (int(x0 * w), int(y0 * h), int((x0 + span) * w), int((y0 + span) * h))
            hands = select(self._scan(arr, box))
            best = summarize(hands)
        else:
            full = self._scan(arr, (0, 0, w, h))
            hands = select(full)
            best = summarize(hands)
            if best["finger_count"] is None:
                boxes = []
                if full:
                    for hand in sorted(full, key=lambda hand: _hand_area(hand), reverse=True):
                        pts = hand["landmarks"]
                        xs = [p["x"] for p in pts]
                        ys = [p["y"] for p in pts]
                        cx = (min(xs) + max(xs)) / 2
                        cy = (min(ys) + max(ys)) / 2
                        sw = min(0.7, max(0.28, 3 * (max(xs) - min(xs))))
                        sh = min(0.7, max(0.28, 3 * (max(ys) - min(ys))))
                        boxes.append(_crop_box(w, h, cx, cy, sw, sh))
                if photo:
                    boxes += [
                        _crop_box(w, h, cx, cy, 0.62, 0.62)
                        for cx, cy in ((0.5, 0.70), (0.30, 0.70), (0.70, 0.70), (0.5, 0.3))
                    ]
                elif source in ("browser", "esp32"):
                    boxes.append(_crop_box(w, h, 0.5, 0.5, 0.72, 0.80))
                boxes = boxes[: int(setting("MAX_CROP_RETRIES", 4))] if photo else boxes[:1]
                candidates = []
                for box in boxes:
                    trial = select(self._scan(arr, box))
                    summary = summarize(trial)
                    if summary["finger_count"] is not None:
                        candidates.append((max(_hand_area(hand) for hand in trial), trial, summary))
                if candidates:
                    _, hands, best = max(candidates, key=lambda item: item[0])
        scale = min(1, int(setting("MAX_SIDE", 1280)) / max(h, w))
        if scale < 1:
            arr = cv2.resize(arr, (int(w * scale), int(h * scale)))
        ok, jpg = cv2.imencode(".jpg", arr)
        if not ok:
            raise ValueError("Không mã hóa được ảnh")
        return hands, best, jpg.tobytes()


def _hand_area(hand):
    pts = hand.get("landmarks") or []
    if not pts:
        return 0
    xs = [p["x"] for p in pts]
    ys = [p["y"] for p in pts]
    return (max(xs) - min(xs)) * (max(ys) - min(ys))


def _crop_box(w, h, cx, cy, sw, sh):
    x0 = max(0, min(1 - sw, cx - sw / 2))
    y0 = max(0, min(1 - sh, cy - sh / 2))
    return (int(x0 * w), int(y0 * h), int((x0 + sw) * w), int((y0 + sh) * h))
