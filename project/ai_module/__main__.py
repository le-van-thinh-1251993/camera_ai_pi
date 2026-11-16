"""Chạy module AI: python -m ai_module  (từ thư mục project / ~/ai-camera-release)."""

import logging

import uvicorn

from ai_module import config
from ai_module.server import create_app


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    cfg = config.load()
    if not cfg.ready:
        logging.warning("Chưa có GEMINI_API_KEY trong %s - /v1/meaning sẽ trả 503", config.ENV_FILE)
    for model in (cfg.model, cfg.fallback_model):
        problem = model and config.text_model_problem(model)
        if problem:
            logging.warning("%s - sửa GEMINI_MODEL / GEMINI_FALLBACK_MODEL trong .env.ai", problem)
    uvicorn.run(create_app(cfg), host=cfg.host, port=cfg.port, log_level="warning")


if __name__ == "__main__":
    main()
