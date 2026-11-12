$ErrorActionPreference = "Stop"

$env:HOST = "0.0.0.0"
$env:PORT = "8000"
$env:LOCAL_ONLY = "1"
$env:CLOUD_ENABLED = "0"
$env:CLOUD_LOCAL_FALLBACK = "1"
# Test local: dashboard khong hoi khoa. Dung run_local.ps1 / dat API_KEY khi mo ra ngoai.
$env:API_KEY = ""
$env:ALLOW_OPEN_DASHBOARD = "1"
$env:LAN_API_KEY = "local-lan-key"
$env:CLOUD_DEVICE_ID = "esp32-01"

Write-Host "Dashboard: http://127.0.0.1:8000 (khong can khoa)"
Write-Host "LAN key for ESP32: local-lan-key"
$python = Join-Path $PSScriptRoot ".venv\Scripts\python.exe"
if (!(Test-Path $python)) { $python = Join-Path (Split-Path $PSScriptRoot -Parent) ".venv\Scripts\python.exe" }
if (!(Test-Path $python)) { $python = "python" }
& $python scripts\serve.py
