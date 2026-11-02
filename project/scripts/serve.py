import os, sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
os.chdir(root)
sys.path.insert(0, str(root))
from dotenv import load_dotenv

load_dotenv(root / ".env", override=True)
import uvicorn

if __name__ == "__main__":
    uvicorn.run(
        "app.main:app",
        host=os.getenv("HOST", "0.0.0.0"),
        port=int(os.getenv("PORT", "8000")),
        workers=1,
        limit_concurrency=40,
        timeout_keep_alive=10,
        timeout_graceful_shutdown=5,
    )
