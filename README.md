# AI Camera Cloud v6

Hệ thống nhận diện cử chỉ đếm ngón tay (1–10) gồm ba phần nối với nhau:

| Thành phần | Vai trò |
| --- | --- |
| **Bo ESP32-S3** (`project/firmware/HybridTerminal`) | Camera OV3660 + TFT 3,5" ILI9488 + loa MAX98357A + 4 nút. Xem live, chụp, gửi ảnh, phát câu nói kết quả. |
| **Raspberry Pi** (`project/app`, hostname `Pi-AI`) | Server FastAPI + worker MediaPipe/OpenCV, dashboard web, màn ST7789 trạng thái, ngrok. |
| **Supabase Cloud** (`migrations/`) | Hàng đợi request, kết quả, ảnh, âm thanh; dữ liệu thống kê lâu dài. |

**Ảnh tải lên, camera điện thoại và ESP32, Auto và Manual đều có đường Cloud và LAN.** Khi có mạng, mọi nguồn ưu tiên Supabase. Khi Supabase/Internet mất, Pi nhận diện trực tiếp và ESP32 chuyển sang LAN; khi mạng trở lại, outbox đồng bộ ngược lên Supabase.

```text
ESP32 ──(Cloud: Supabase RPC/Storage)──► Supabase ◄── worker trên Pi
  │                                                    │
  └──(LAN fallback: http://Pi-AI.local:8000)──────────► Pi ──► dashboard (LAN / ngrok HTTPS)
                                                       └──► màn ST7789 trạng thái
```

## Cấu trúc thư mục

| Đường dẫn | Nội dung |
| --- | --- |
| `install_pi.sh`, `install.py` | Installer/rollback trên Pi |
| `migrations/` | SQL Supabase 001–004 |
| `project/app/` | Server, worker Cloud, fallback LAN, retention, TTS |
| `project/static/` | Dashboard (JS tách module: camera, upload, cloud, audio, device, events, stats) |
| `project/firmware/HybridTerminal/` | Firmware ESP32 duy nhất |
| `project/firmware/TftPinTest/` | Sketch thử chân/tốc độ TFT |
| `project/scripts/` | `pi_lcd.py`, `verify_cloud.py`, `diagnose_cloud.py`, `dashboard_key.py`, `make_tft_assets.py`, `import_voice.py`… |
| `project/systemd/` | Mẫu service `ai-camera`, `ai-camera-ngrok`, `ai-camera-lcd` |
| `anh bo mach/` | Ảnh bo mạch thật + `Bang_Ket_Noi_Module_Schematic.xlsx` (bảng nối chân) |
| `voice/voice.rar` | Bộ giọng gốc; `doi-cau-noi-va-voice.patch` là patch đổi câu nói |
| `VALIDATION.md`, `REVIEW_FIXES.md`, `reports/` | Kết quả kiểm thử và giới hạn |

## Cài đặt / cập nhật trên Pi

```bash
git clone https://github.com/le-van-thinh-1251993/camera_ai_pi.git ~/ai-camera-cloud-v6   # hoặc git pull nếu đã có
bash ~/ai-camera-cloud-v6/install_pi.sh
```

Installer dùng venv `~/ai-camera/.venv`, giữ model/recording, ánh xạ câu nói, `.env.cloud`, các key Supabase và `secrets.h` hiện có. Chỉ cài thư viện còn thiếu, không thay MediaPipe/OpenCV. Source cập nhật tại `~/ai-camera-release`; file bị thay có backup trong `backups/cloud-v6-*`. Installer dừng worker cũ trước khi chép source và bật `ai-camera.service` khi Pi khởi động.

**Không commit/đóng gói secret Supabase.** `SUPABASE_SECRET_KEY` có toàn quyền Storage/Postgres, chỉ đặt trong `.env.cloud` của máy đích. Nếu `API_KEY` dashboard rỗng, installer sinh khóa ngẫu nhiên; đọc khóa trước khi mở ngrok công khai:

```bash
~/ai-camera/.venv/bin/python ~/ai-camera-release/scripts/dashboard_key.py --show
```

Mở dashboard, **Ctrl+F5**, nhập khóa vào cửa sổ Cài đặt kết nối. Camera iPhone cần mở qua link ngrok **HTTPS**, cấp quyền camera rồi chọn camera trước/sau.

### SQL Supabase

Project mới: chạy schema gốc `001`, rồi `002`, `003`, `004` theo thứ tự. Project đã áp dụng thì không cần chạy lại. `003` thêm input nhiều khung, RPC gửi request/trao đổi trạng thái, âm thanh dùng chung và lỗi vĩnh viễn; `004` tách claim v5/v6 để worker cũ không nhận nhầm job mới. Migration không đổi key.

### Tự khởi động ngrok

Nếu có ngrok ở `/usr/bin/ngrok` hoặc `~/ngrok-demo/ngrok`, installer tạo thêm `ai-camera-ngrok.service`; token đọc từ cấu hình ngrok hiện có.

```bash
curl http://127.0.0.1:4040/api/tunnels                     # xem link public
sudo systemctl status ai-camera ai-camera-ngrok --no-pager
sudo systemctl disable --now ai-camera-ngrok               # tắt tunnel nếu chưa muốn public
```

## Mạng LAN và hostname Pi

Từ 15/11/2026 Pi dùng hostname **`Pi-AI`** (mDNS `Pi-AI.local`, trước đây là `ai-camera.local`). Firmware (`LAN_HOSTNAME` trong `config.h`) đã đổi theo. Endpoint LAN:

```text
http://Pi-AI.local:8000
```

```bash
sudo apt-get install -y avahi-daemon
sudo hostnamectl set-hostname Pi-AI
sudo systemctl enable --now avahi-daemon
getent hosts Pi-AI.local
```

> `project/.env.example` vẫn ghi `LAN_HOSTNAME=ai-camera.local`; nếu dùng biến này trên Pi, sửa thành `Pi-AI.local`.

ESP32 tìm máy chủ LAN theo thứ tự: IP đặt trên trang cấu hình WiFi / `LAN_HOST_IP_VALUE` trong `secrets.h` → mDNS `Pi-AI.local` → dò UDP cổng 8002 (`app/lan_discovery.py`, dùng được với máy Windows không có mDNS) → gateway. Không dùng IP cứng hay link ngrok cho LAN fallback; ngrok cần Internet.

Đặt `LAN_API_KEY` trong `.env` trên Pi và chép cùng giá trị vào `secrets.h` (`LAN_API_KEY_VALUE`); để trống thì server dùng lại `API_KEY`. Khi Cloud lỗi, ESP32 gửi JPEG, trạng thái, ACK và nhận kết quả/audio qua LAN. Dữ liệu lưu ở `data/hybrid/jobs.sqlite3`; worker upload lại ảnh, audio, kết quả khi Cloud hồi phục. `CLOUD_LOCAL_FALLBACK=1` là mặc định; đặt `0` chỉ khi muốn Cloud-only.

## Dashboard web

- Hai nguồn camera: điện thoại/máy đang mở trang, và ESP32 qua Cloud/LAN. Thêm tab tải ảnh.
- **Auto giữ 5 giây** (`HOLD_SECONDS` trong `static/js/api.js`, `AUTO_HOLD_SECONDS` trên server, `AUTO_HOLD_MS` trong firmware — ba giá trị phải khớp). Đếm ngược trên thiết bị rồi gửi **đúng một ảnh**; có kết quả thì dừng và phát clip, bấm **Nhận diện tiếp** cho lượt mới. Manual và Upload gửi một ảnh.
- Với ESP32, dòng trạng thái dưới khung camera mô tả việc bo đang làm thật: chưa kết nối / đang chụp / chờ bo nhận lệnh / AUTO đang đếm ngược / MANUAL chờ bấm chụp / đã có kết quả / bo ở màn chào.
- Live ESP32: Pi chuyển tiếp MJPEG `http://<ip-esp32>:81/stream` qua `/api/esp32/live/stream` (đặt `ESP32_LIVE_HOST` nếu không dùng `esp32-01.local`).
- **Cộng tất cả tay** để đếm 6–10 ngón bằng hai tay; mặc định chỉ lấy tay nổi bật. **Chọn tay trong ảnh** khi ảnh có nhiều người. Nắm tay 0 ngón tính là chưa nhận diện (bộ clip chỉ có 1–10).
- Nút chạm ≥ 44 px, chế độ sáng/tối, **Phóng to ảnh**. Camera điện thoại tạm dừng khi ẩn trang để tránh ảnh stale trên iOS.

### Chạy local không cần Supabase (Windows)

```powershell
cd project
.\run_local.ps1          # hoặc .\run_local_lan.ps1 để ESP32 trong LAN gọi tới
```

Chế độ `LOCAL_ONLY=1`: nhận diện bằng MediaPipe trên máy, lưu SQLite, mở `http://127.0.0.1:8000`.

## Màn ST7789 trạng thái trên Pi

`project/scripts/pi_lcd.py` vẽ bảng trạng thái lên màn SPI ST7789 gắn trên Pi (hỗ trợ Pi 5, `lgpio` + `spidev`): tên/IP Pi, WiFi, web, Supabase, ESP32 (online, AUTO/MANUAL), kết quả gần nhất và địa chỉ dashboard. Chỉ đọc server local `http://127.0.0.1:8000`, không gọi Internet.

Nối mặc định (BCM): SPI0 (MOSI GPIO10, SCLK GPIO11, CE0), `DC=25`, `RST=27`, `BL=24`. Bật SPI bằng `raspi-config`. Cấu hình trong `.env` (đều có mặc định):

```dotenv
LCD_WIDTH=240           # 240x240, 240x280, 240x320, 172x320, 135x240
LCD_HEIGHT=240
LCD_ROTATION=0          # 0/1/2/3
LCD_INVERT=1            # màu âm bản -> 0
LCD_SPI_MODE=0          # màn 7 chân không có CS -> 3
LCD_SPI_HZ=40000000     # nhiễu -> 20000000
LCD_DC=25
LCD_RST=27
LCD_BL=24               # -1 nếu BLK nối thẳng 3.3V
LCD_BGR=0               # đỏ/xanh dương bị đảo -> 1
# LCD_X_OFFSET / LCD_Y_OFFSET chỉ đặt khi hình bị lệch
```

```bash
cd ~/ai-camera-release
~/ai-camera/.venv/bin/python scripts/pi_lcd.py --test          # 4 dải màu + khung vàng
~/ai-camera/.venv/bin/python scripts/pi_lcd.py --once          # vẽ bảng 1 lần
~/ai-camera/.venv/bin/python scripts/pi_lcd.py --png lcd.png   # không cần màn, xuất PNG
```

Installer **chưa** cài service màn; cài tay từ mẫu:

```bash
cd ~/ai-camera-release
sed -e "s|@USER@|$(id -un)|" -e "s|@ROOT@|$PWD|" -e "s|@PYTHON@|$HOME/ai-camera/.venv/bin/python|" \
  systemd/ai-camera-lcd.service.in | sudo tee /etc/systemd/system/ai-camera-lcd.service >/dev/null
sudo systemctl daemon-reload && sudo systemctl enable --now ai-camera-lcd
```

## Firmware ESP32

Dùng duy nhất `project/firmware/HybridTerminal`. Giữ `secrets.h` của thiết bị; `secrets.example.h` chỉ là mẫu trống.

Arduino IDE: core **esp32 3.0.x**, thư viện **LovyanGFX**, board *ESP32S3 Dev Module*, `PSRAM: OPI PSRAM`, `Flash Size: 16MB`, `Partition: 16M Flash (3MB APP/9.9MB FATFS)`.

### Nối TFT 3,5" (bo đã sửa, `SHARED_CAM_TFT_BUS 0`)

Chân 8–12 của socket TFT được cắt khỏi bus camera và nối lại. Cấu hình hiện tại đã thử ổn bằng dây dupont (15/11/2026):

| TFT | Nối tới | Ghi chú |
| --- | --- | --- |
| SCK | GPIO1 | |
| SDI/MOSI | GPIO2 | chung đường LED đỏ trên bo |
| DC/RS | GPIO48 | chung LED RGB WS2812 nên LED sáng; dự kiến chuyển sang GPIO40 khi hàn lại |
| CS | **GPIO39** | firmware điều khiển (`TFT_CS_PIN`) |
| RESET | **GPIO40** | firmware điều khiển (`TFT_RST_PIN`) |
| VCC | **5V trực tiếp** | qua công tắc trên mạch in làm màn trắng |

CS nối GND / RESET nối EN (cách cũ) làm màn trắng do xung nhiễu SCK lúc khởi động; muốn dùng lại cách đó thì đặt `TFT_CS_PIN` và `TFT_RST_PIN` thành `-1` trong `config_35inch_tft.h`. GPIO39/40 là chân mic INMP441 — muốn dùng mic phải đổi chân khác.

SPI: lệnh khởi tạo gửi ở 8 MHz, sau đó vẽ ở **20 MHz** (~97 ms/khung, nhanh nhất đo được; 26/40 MHz bị chia xung còn chậm hơn). Có thể đổi tốc độ lúc chạy qua `/cam?spi=`.

**Màn trắng khi đang chạy:** giữ cùng lúc **DOWN + UP** (~1,2 s một lần) hoặc gõ `r` trên Serial để đồng bộ lại (chèn 1 xung SCK, reset mềm, init lại; tối đa 8 lần là khớp). `TftPinTest` có lệnh `a` thử cả 8 vị trí lệch nhịp để phân biệt lỗi lệch nhịp với lỗi dây/nguồn.

Bo **chưa sửa**: đặt `SHARED_CAM_TFT_BUS 1`, TFT dùng chung GPIO 8–12 với camera và chạy luân phiên từng khung (chậm hơn nhiều).

### Camera OV3660

Chân (`config.h`): `XCLK=15`, `SIOD=4`, `SIOC=5`, `Y9..Y2 = 16, 17, 18, 12, 10, 8, 9, 11`, `VSYNC=6`, `HREF=7`, `PCLK=13`, `PWDN=-1`, `RESET=-1`.

Live QVGA 320×240 vẽ 1:1 lên TFT và phát cùng khung ở cổng 81; ảnh gửi Pi là VGA 640×480. Tham số ảnh mặc định chỉnh cho môi trường ngược sáng: `GAINCEILING_32X`, `AE_LEVEL=4` (OV3660 cho −5..5), `BRIGHTNESS=2`, `SATURATION=2`, `CONTRAST=1`, `SHARPNESS=2`, `DENOISE=3`, cân bằng trắng tự động, không hiệu ứng.

Chỉnh thử khi đang chạy: `http://<ip-esp32>/cam?ae=..&bright=..&contrast=..&sat=..&sharp=..&denoise=..&quality=..` — và đọc/ghi thanh ghi cảm biến trực tiếp `/cam?reg=0x3a0f&val=0x60` (bỏ `val` để chỉ đọc).

### Cấu hình WiFi

- **Khởi động:** thử mạng mặc định trong `secrets.h` **đầu tiên**, rồi các mạng đã lưu (12 giây mỗi mạng). Vào được thì màn chào hiện IP và `esp32-01.local`; mở `http://<ip>/` để xem/đổi WiFi và đặt IP máy chủ nhận diện.
- **Không vào được mạng nào:** phát hotspot mở `AI-Camera-XXXX`, TFT có mã QR; vào hotspot thì trang cấu hình tự bật (hoặc `http://192.168.4.1/`).
- **Mất WiFi khi đang chạy:** tự kết nối lại; quá 30 giây bật hotspot song song (SELECT để bật ngay). Có mạng lại thì hiện IP mới cạnh IP cũ 8 giây, hotspot tắt sau 20 giây.
- **Đổi WiFi chủ động:** giữ BACK 3 giây ở màn chào, giữ BACK khi cấp điện, hoặc gõ `w` trên Serial.

Lưu tối đa 5 mạng trong NVS (mạng mặc định của `secrets.h` không ghi vào NVS); nạp lại firmware không mất danh sách. Trang cấu hình không có mật khẩu: ai trong cùng LAN/hotspot đều đổi được.

### Giao diện TFT và nút

- **Màn chào:** “Xin chào!” + lời chào qua loa. SELECT để vào live.
- **Live:** khung camera 320×240 bên trái, cột phải có thẻ **Kết quả** (nhãn ● LIVE) và **Ý nghĩa**, footer hiện chế độ/chức năng SELECT/âm lượng. Khởi động ở **MANUAL**.
  - **MANUAL:** giơ tay vào khung rồi nhấn SELECT để chụp.
  - **AUTO:** giữ yên tay **5 giây** rồi tự chụp và gửi.
- **Kết quả:** ảnh vừa chụp đứng yên, số ngón và câu ý nghĩa, loa đọc câu; sau 8 giây (`AUTO_NEXT_ROUND_MS`) quay lại live, thẻ đổi thành “LẦN TRƯỚC”.

| Nút | GPIO | Chức năng |
| --- | --- | --- |
| BACK1 | 47 | Đổi AUTO / MANUAL |
| SELECT1 | 21 | Bắt đầu · chụp (MANUAL) · chụp ngay (AUTO) · live tiếp |
| DOWN1 | 3 | Tăng âm lượng |
| UP1 | 14 | Giảm âm lượng |
| DOWN1 + UP1 (giữ) | | Đồng bộ lại TFT khi màn trắng |

Âm lượng được lưu trong NVS (`audio`: `vol`, `prev`) nên khởi động lại vẫn giữ.

Serial Monitor (115200): `b s + -` giả lập 4 nút, `i` in FPS và thời gian chụp/vẽ, `w` mở hotspot WiFi, `r` đồng bộ lại TFT.

Font Noto Sans (SIL OFL) và lời chào nhúng vào firmware bằng `python scripts/make_tft_assets.py` (tạo `fonts_vi.h`, `welcome_audio.h`). Đổi lời chào: `python scripts/make_tft_assets.py --skip-fonts --wav <file.wav>` (WAV mono 16-bit, 8–48 kHz).

Auto gửi một JPEG sau khi đếm ngược, không ghi LittleFS/NVS cho mẫu chưa xác nhận. Chỉ lưu UUID Manual đã gửi lên Cloud (để phục hồi sau reboot), mã kết quả đã tiêu thụ và ACK cuối. Loa phát tối đa một lần mỗi UUID; dùng Replay khi cần. Firmware tái sử dụng TLS socket, gộp báo trạng thái/đọc lệnh trong một RPC, giảm polling khi rảnh, và timeout khi không có khung camera thay vì kẹt vòng Auto.

## Xác thực và ngrok

API dashboard yêu cầu `API_KEY`; endpoint ESP32 LAN dùng `LAN_API_KEY`. App từ chối khởi động khi mở ngoài loopback mà key rỗng. `/health` công khai chỉ trả trạng thái cơ bản; phiên bản và mã lỗi ở `/api/diagnostics` (có xác thực).

Có thể thêm Basic Auth của ngrok bằng mẫu `project/examples/ngrok-traffic-policy.example.yml` (tạo bản riêng, đổi mật khẩu). Không dùng key Supabase làm mật khẩu web. Tham khảo [ngrok Basic Auth](https://ngrok.com/docs/gateway/traffic-policy/actions/basic-auth).

## Dọn dữ liệu Cloud

Kết quả trên Supabase là dữ liệu **thống kê**, nên được giữ tối đa trong giới hạn gói Free (500 MB database, 1 GB Storage; mỗi lần nhận diện ≈ 3,5 KB hàng + ≈ 35 KB ảnh chụp/ảnh màn hình):

- **Hàng kết quả:** giữ 80.000 lần gần nhất (≈ 300–400 MB); vượt thì xóa hàng cũ nhất cùng file.
- **Ảnh:** giữ ảnh của 24.000 lần gần nhất (≈ 850 MB). Lần cũ hơn chỉ bị xóa ảnh, **hàng vẫn giữ** (đánh dấu `input.images_pruned=true`).
- Mẫu Auto không được xác nhận dọn sau 60 phút. Không xóa pending/processing hay clip giọng dùng chung.
- Free project bị tạm dừng nếu 7 ngày không hoạt động; Pi chạy liên tục nên không bị.

Xóa object luôn qua Storage API trước khi xóa hàng PostgreSQL. Biến cấu hình (lên gói Pro thì tăng hai số đầu):

```dotenv
CLOUD_RETENTION_ENABLED=1
CLOUD_KEEP_ROWS=80000
CLOUD_KEEP_IMAGES=24000
CLOUD_RETENTION_DAYS=0              # 0 = không giới hạn tuổi
CLOUD_PROBE_RETENTION_MINUTES=60
MAX_CROP_RETRIES=4
```

WAV dùng chung theo SHA-256. Ảnh hỏng/4xx đánh dấu failed ngay; lỗi mạng/503 có retry và lease fencing. Polling giãn khi rảnh, được đánh thức khi có request web hoặc lệnh mới.

## Kiểm tra

```bash
curl -s http://127.0.0.1:8000/health
cd ~/ai-camera-release
~/ai-camera/.venv/bin/python scripts/verify_cloud.py     # ghi data/validation/verify-cloud.json
~/ai-camera/.venv/bin/python scripts/diagnose_cloud.py
sudo journalctl -u ai-camera -n 40 --no-pager

~/ai-camera/.venv/bin/python -m unittest discover -s tests/v6 -v
~/ai-camera/.venv/bin/python -m unittest discover -s tests -p test_counter.py -v
```

`verify_cloud.py` thử ảnh mẫu thật qua các route Cloud (upload, Manual, Auto một ảnh, WAV, ảnh lỗi); camera trong script là dữ liệu mô phỏng, không chứng minh iPhone/ESP32 vật lý hoạt động.

JS: Node 22, `npm ci --ignore-scripts`, `npm test`. Có Prettier, Ruff và workflow CI (`.github/workflows/tests.yml`). Kiểm thử trình duyệt tùy chọn: `tests/browser_cloud.py` qua Chrome headless `--remote-debugging-port` (không mở cổng debug ra Internet). Xem `VALIDATION.md` cho kết quả thực chạy và giới hạn.

## Khôi phục

```bash
sudo systemctl stop ai-camera
~/ai-camera/.venv/bin/python ~/ai-camera-cloud-v6/install.py --rollback ~/ai-camera-release/backups/cloud-v6-THOI_GIAN
sudo systemctl start ai-camera
```

Dùng đúng đường dẫn backup installer đã in. Rollback không xóa dữ liệu Cloud và không đổi key.
