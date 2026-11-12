$ErrorActionPreference = "Stop"

$env:HOST = "127.0.0.1"
$env:PORT = "8000"
$env:LOCAL_ONLY = "1"
$env:CLOUD_ENABLED = "0"
$env:CLOUD_LOCAL_FALLBACK = "1"
$env:API_KEY = ""
$env:LAN_API_KEY = ""
$env:CLOUD_DEVICE_ID = "esp32-01"

$python = Join-Path $PSScriptRoot ".venv\Scripts\python.exe"
if (!(Test-Path $python)) { $python = Join-Path (Split-Path $PSScriptRoot -Parent) ".venv\Scripts\python.exe" }
if (!(Test-Path $python)) { $python = "python" }
& $python scripts\serve.py
