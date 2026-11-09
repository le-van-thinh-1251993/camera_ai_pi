$ErrorActionPreference = "Stop"

$env:HOST = "0.0.0.0"
$env:PORT = "8000"
$env:LOCAL_ONLY = "1"
$env:CLOUD_ENABLED = "0"
$env:CLOUD_LOCAL_FALLBACK = "1"
$env:API_KEY = "local-dashboard-key"
$env:LAN_API_KEY = "local-lan-key"
$env:CLOUD_DEVICE_ID = "esp32-01"

Write-Host "Dashboard: http://127.0.0.1:8000"
Write-Host "LAN key for ESP32: local-lan-key"
$python = Join-Path (Split-Path $PSScriptRoot -Parent) ".venv\Scripts\python.exe"
if (!(Test-Path $python)) { $python = "python" }
& $python scripts\serve.py
