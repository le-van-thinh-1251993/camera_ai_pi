"""Gọi Gemini (REST generateContent) để nghĩ một câu ý nghĩa cho số ngón tay.

Chỉ dùng httpx (không cần SDK). Lỗi được quy về AiError(code) để core ghi lại và dùng
câu cố định; key không bao giờ xuất hiện trong thông báo lỗi.
"""

import base64
import json

import httpx

from ai_module import quota

API = "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"

SYSTEM = (
    "Bạn là trợ lý của một thiết bị camera nhận diện cử chỉ đếm ngón tay. "
    "Nhiệm vụ: viết MỘT câu tiếng Việt ngắn, tối đa {max_chars} ký tự, nói về ý nghĩa "
    "của số ngón tay người dùng vừa giơ. Phong cách: {style}. "
    "Có thể nhắc nhẹ tới khung cảnh chung trong ảnh (ánh sáng, đồ vật, màu sắc) nếu tự nhiên. "
    "Quy tắc bắt buộc: số ngón do hệ thống đếm là đúng, không tự đếm lại; "
    "không nhận xét danh tính, tên, tuổi, giới tính, dân tộc, ngoại hình, cơ thể hay cảm xúc "
    "của người trong ảnh; không nội dung nhạy cảm, chính trị, tôn giáo; "
    "không emoji, không ký hiệu đặc biệt, không xuống dòng; câu phải đọc to tự nhiên."
)

SCHEMA = {
    "type": "OBJECT",
    "properties": {"y_nghia": {"type": "STRING"}},
    "required": ["y_nghia"],
}


class AiError(Exception):
    """code: quota | auth | config | blocked | timeout | network | bad_response."""

    def __init__(self, code, detail=""):
        super().__init__(code)
        self.code = code
        self.detail = detail


def build_request(cfg, finger_count, image_jpeg, model=None):
    parts = []
    if image_jpeg:
        parts.append(
            {
                "inlineData": {
                    "mimeType": "image/jpeg",
                    "data": base64.b64encode(image_jpeg).decode("ascii"),
                }
            }
        )
    parts.append(
        {
            "text": f"Hệ thống vừa đếm được {finger_count} ngón tay. "
            "Hãy viết câu ý nghĩa theo đúng quy tắc."
        }
    )
    generation = {
        "responseMimeType": "application/json",
        "responseSchema": SCHEMA,
        "temperature": 1.0,
        "maxOutputTokens": 512,
    }
    # Chỉnh mức suy nghĩ chỉ cho model chính (đo thật: gemini-3.5-flash 5,2 s -> 2,4 s với
    # "low"). Model dự phòng gửi yêu cầu tối giản để ít khả năng bị từ chối tham số.
    name = model or cfg.model
    if name == cfg.model:
        if name.startswith("gemini-2.5") and cfg.thinking_budget not in ("", "auto"):
            try:
                generation["thinkingConfig"] = {"thinkingBudget": int(cfg.thinking_budget)}
            except ValueError:
                pass
        elif not name.startswith("gemini-2") and cfg.thinking_level:
            generation["thinkingConfig"] = {"thinkingLevel": cfg.thinking_level}
    return {
        "systemInstruction": {
            "parts": [{"text": SYSTEM.format(max_chars=cfg.max_chars, style=cfg.style)}]
        },
        "contents": [{"role": "user", "parts": parts}],
        "generationConfig": generation,
    }


def parse_response(data):
    feedback = (data.get("promptFeedback") or {}).get("blockReason")
    candidates = data.get("candidates") or []
    if feedback or not candidates:
        raise AiError("blocked", feedback or "no_candidates")
    first = candidates[0]
    if first.get("finishReason") in ("SAFETY", "PROHIBITED_CONTENT", "BLOCKLIST", "SPII"):
        raise AiError("blocked", first["finishReason"])
    text = "".join(
        part.get("text", "")
        for part in (first.get("content") or {}).get("parts") or []
        if not part.get("thought")
    ).strip()
    if text.startswith("```"):
        text = text.strip("`").removeprefix("json").strip()
    try:
        meaning = json.loads(text)["y_nghia"]
    except (ValueError, KeyError, TypeError):
        raise AiError("bad_response", text[:80]) from None
    if not isinstance(meaning, str) or not meaning.strip():
        raise AiError("bad_response", "empty")
    return meaning


# Lỗi đáng thử lại bằng model dự phòng (auth/blocked thì đổi model cũng vô ích).
RETRY_WITH_FALLBACK = {"quota", "timeout", "network", "config", "bad_response"}


async def meaning_with_fallback(cfg, finger_count, image_jpeg, client=None):
    """Thử model chính; lỗi tạm thời thì thử GEMINI_FALLBACK_MODEL. -> (câu, model đã dùng)."""
    try:
        return await meaning(cfg, finger_count, image_jpeg, client), cfg.model
    except AiError as exc:
        if (
            not cfg.fallback_model
            or cfg.fallback_model == cfg.model
            or exc.code not in RETRY_WITH_FALLBACK
        ):
            raise
        first = exc
    try:
        text = await meaning(
            cfg, finger_count, image_jpeg, client, cfg.fallback_model, cfg.fallback_timeout
        )
        return text, cfg.fallback_model
    except AiError as exc:
        exc.detail = f"{cfg.model}: {first.code}; {cfg.fallback_model}: {exc.detail}"
        raise


async def meaning(cfg, finger_count, image_jpeg, client=None, model=None, timeout=None):
    """Trả về câu ý nghĩa (chưa làm sạch). client: httpx.AsyncClient để test."""
    if not cfg.api_key:
        raise AiError("config", "missing GEMINI_API_KEY")
    model = model or cfg.model
    if quota.blocked(model):  # đã hết lượt hôm nay: khỏi gọi thử, sang model dự phòng ngay
        raise AiError("quota", f"{model} hết lượt, chờ reset")
    body = build_request(cfg, finger_count, image_jpeg if cfg.send_image else None, model)
    own = client is None
    timeout = timeout or cfg.gemini_timeout
    client = client or httpx.AsyncClient(timeout=timeout)
    try:
        response = await client.post(
            API.format(model=model or cfg.model),
            timeout=timeout,
            headers={"x-goog-api-key": cfg.api_key, "Content-Type": "application/json"},
            json=body,
        )
    except httpx.TimeoutException:
        raise AiError("timeout") from None
    except httpx.HTTPError as exc:
        raise AiError("network", type(exc).__name__) from None
    finally:
        if own:
            await client.aclose()
    if response.status_code == 429:
        quota.block(model, response)
        raise AiError("quota", "HTTP 429")
    if response.status_code in (401, 403):
        raise AiError("auth", f"HTTP {response.status_code}")
    if response.status_code in (400, 404):
        message = ""
        try:
            message = response.json().get("error", {}).get("message", "")[:160]
        except ValueError:
            pass
        raise AiError("config", f"HTTP {response.status_code} {message}")
    if response.status_code >= 300:
        raise AiError("network", f"HTTP {response.status_code}")
    try:
        data = response.json()
    except ValueError:
        raise AiError("bad_response", "not_json") from None
    return parse_response(data)
