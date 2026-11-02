"""Shared validation and the two-second Auto decision, independent of network latency."""

import math
import uuid

HOLD_MS = 2000


def validate_input(request_id, owner, primary, data):
    rid = str(uuid.UUID(str(request_id)))
    uid = str(uuid.UUID(str(owner)))
    if primary != f"{uid}/{rid}.jpg":
        raise ValueError("invalid_image_path")
    mode = data.get("mode", "manual")
    capture_mode = data.get("capture_mode", mode)
    if mode not in ("auto", "manual", "upload"):
        raise ValueError("invalid_mode")
    if capture_mode not in ("auto", "manual", "upload"):
        raise ValueError("invalid_capture_mode")
    frames = data.get("frames") or [{"path": primary, "ms": 0}]
    # New single-image Auto uses mode=manual plus capture_mode=auto. Keep
    # mode=auto with three frames readable for jobs created before v6 single.
    legacy_batch = mode == "auto" and capture_mode == "auto"
    if len(frames) != (3 if legacy_batch else 1):
        raise ValueError("invalid_frame_count")
    allowed = {primary, f"{uid}/{rid}/1.jpg", f"{uid}/{rid}/2.jpg"}
    previous = -1
    for frame in frames:
        ms = frame.get("ms")
        if (
            frame.get("path") not in allowed
            or not isinstance(ms, int)
            or not 0 <= ms <= 6000
            or ms <= previous
        ):
            raise ValueError("invalid_capture_frame")
        previous = ms
    if frames[0]["path"] != primary:
        raise ValueError("invalid_primary_frame")
    if legacy_batch and frames[-1]["ms"] - frames[0]["ms"] < HOLD_MS:
        raise ValueError("hold_too_short")
    focus = data.get("focus")
    if focus is not None and (
        not isinstance(focus, list)
        or len(focus) != 2
        or not all(isinstance(v, (int, float)) and math.isfinite(v) and 0 <= v <= 1 for v in focus)
    ):
        raise ValueError("invalid_focus")
    return frames


def auto_decision(events, frames):
    """Both endpoints must agree; tolerate one occluded middle sample, never a different valid gesture."""
    if len(events) != 3 or frames[-1]["ms"] - frames[0]["ms"] < HOLD_MS:
        return False
    first = events[0].get("finger_count")
    last = events[-1].get("finger_count")
    if first is None or first != last or not 1 <= first <= 10:
        return False
    middle = events[1].get("finger_count")
    return middle in (None, first)


def is_permanent(exc):
    code = getattr(exc, "status_code", None) or getattr(exc, "status", None)
    return isinstance(exc, (ValueError, TypeError)) or code in (400, 404, 409, 413, 415, 422)
