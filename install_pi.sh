#!/usr/bin/env bash
set -euo pipefail
BUNDLE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HOME/ai-camera-release"
PY="$HOME/ai-camera/.venv/bin/python"
if [[ ! -x "$PY" ]]; then echo "Missing $PY. Use the existing MediaPipe venv; do not create a replacement blindly."; exit 1; fi
"$PY" -c 'import sys,cv2,mediapipe; print(sys.version.split()[0],"MediaPipe",mediapipe.__version__,"OpenCV",cv2.__version__)'
if ! "$PY" -c 'import fastapi,uvicorn,multipart,dotenv,PIL,httpx' >/dev/null 2>&1; then "$PY" -m pip install -r "$BUNDLE/project/requirements.txt"; fi
if [[ ! -f "$ROOT/.env.cloud" ]]; then echo "Missing existing .env.cloud; no source or keys changed."; exit 1; fi
sudo -v
if [[ ! -f /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf ]]; then sudo apt-get install -y fonts-dejavu-core; fi
if systemctl cat ai-camera.service >/dev/null 2>&1; then
  sudo mkdir -p /etc/systemd/system/ai-camera.service.d
  printf '[Service]\nTimeoutStopSec=12\nKillMode=mixed\n' | sudo tee /etc/systemd/system/ai-camera.service.d/10-hybrid-shutdown.conf >/dev/null
  sudo systemctl daemon-reload
  sudo systemctl stop ai-camera.service
fi
sudo systemctl stop ai-camera-ngrok.service 2>/dev/null || true
"$PY" "$BUNDLE/install.py" --target "$ROOT"
"$PY" - "$ROOT" "$PY" "$(id -un)" <<'PY'
import sys
from pathlib import Path
root,python,user=sys.argv[1:]
path=Path(root)/'systemd/ai-camera.service.in'
text=path.read_text().replace('@USER@',user).replace('@ROOT@',root).replace('@PYTHON@',python)
(Path(root)/'systemd/ai-camera.service').write_text(text)
PY
sudo cp "$ROOT/systemd/ai-camera.service" /etc/systemd/system/ai-camera.service
sudo systemctl daemon-reload
sudo systemctl enable --now ai-camera.service

# Start ngrok automatically when an existing Pi installation has ngrok and an
# auth token configured. The token remains in ~/.config/ngrok and is never
# copied into this release ZIP.
NGROK_BIN="${NGROK_BIN:-}"
if [[ -z "$NGROK_BIN" ]] && command -v ngrok >/dev/null 2>&1; then
  NGROK_BIN="$(command -v ngrok)"
fi
if [[ -z "$NGROK_BIN" ]] && [[ -x "$HOME/ngrok-demo/ngrok" ]]; then
  NGROK_BIN="$HOME/ngrok-demo/ngrok"
fi
if [[ -n "$NGROK_BIN" ]]; then
  "$PY" - "$ROOT" "$NGROK_BIN" "$HOME" "$(id -un)" <<'PY'
import sys
from pathlib import Path
root, binary, home, user = sys.argv[1:]
template = (Path(root) / "systemd/ai-camera-ngrok.service.in").read_text()
text = (template.replace("@USER@", user)
        .replace("@HOME@", home)
        .replace("@NGROK_HOME@", str(Path(binary).parent))
        .replace("@NGROK_BIN@", binary))
(Path(root) / "systemd/ai-camera-ngrok.service").write_text(text)
PY
  sudo cp "$ROOT/systemd/ai-camera-ngrok.service" /etc/systemd/system/ai-camera-ngrok.service
  sudo systemctl daemon-reload
  sudo systemctl enable --now ai-camera-ngrok.service
  echo "ngrok tự khởi động cùng Pi; xem link bằng: curl http://127.0.0.1:4040/api/tunnels"
else
  echo "Không tìm thấy ngrok; dashboard local vẫn tự khởi động. Cài ngrok rồi chạy lại installer để bật tunnel tự động."
fi
for attempt in {1..25}; do
  health="$(curl -fsS --max-time 2 http://127.0.0.1:8000/health 2>/dev/null || true)"
  if [[ -n "$health" ]] && "$PY" - "$health" <<'PY'
import json, sys
try:
    value = json.loads(sys.argv[1])
except (ValueError, TypeError):
    raise SystemExit(1)
raise SystemExit(0 if value.get("ready") and value.get("cloud_ready") else 1)
PY
  then
    printf '%s\nReady. Refresh browser with Ctrl+F5.\n' "$health"
    exit 0
  fi
  sleep 1
done
echo "Dịch vụ đã khởi động nhưng Supabase chưa sẵn sàng; xem lỗi bên dưới."
sudo journalctl -u ai-camera -n 40 --no-pager
exit 1
