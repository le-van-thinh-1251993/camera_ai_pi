# AI Camera Cloud v6

Bản nâng cấp theo review ngày 30/10/2026. **Ảnh tải lên, camera điện thoại và ESP32, Auto và Manual đều có đường Cloud và LAN.** Khi Supabase mất kết nối, Pi nhận diện trực tiếp bằng MediaPipe/OpenCV; khi mạng trở lại, outbox đồng bộ ngược lên Supabase.

## Cài bản cập nhật

Trên Windows PowerShell, chép ZIP mới lên Pi rồi SSH:

```powershell
scp "C:\Users\minh.nl\Documents\Codex\2026-10-22\referenced-chatgpt-conversation-this-is-an\outputs\ai-camera-cloud-v6-single-network-fix.zip" lethinh@ai-camera.local:~/
ssh lethinh@ai-camera.local
```

Nếu ZIP ở thư mục khác, dùng đúng đường dẫn thực tế. Trên Pi:

```bash
unzip -o ~/ai-camera-cloud-v6-single-network-fix.zip -d ~/ai-camera-cloud-v6-single-network-fix
bash ~/ai-camera-cloud-v6-single-network-fix/install_pi.sh
```

Installer dùng venv `~/ai-camera/.venv`, giữ model/recording, ánh xạ câu nói, `.env.cloud`, các key Supabase và `secrets.h` hiện có. Chỉ cài thư viện còn thiếu, không thay MediaPipe/OpenCV. Source cập nhật tại `~/ai-camera-release`; file bị thay có backup trong `backups/cloud-v6-*`.

**Không đưa secret/service key Supabase vào ZIP.** ZIP có sẵn URL project, còn `.env.cloud` và khóa server hiện có trên Pi được giữ nguyên. Secret Supabase có toàn quyền Storage/Postgres; hardcode vào ZIP sẽ làm lộ quyền truy cập khi chép sang máy khác. `SUPABASE_SECRET_KEY` vẫn phải được cung cấp trong `.env.cloud` của máy đích. Nếu `API_KEY` dashboard đang rỗng, installer sinh khóa ngẫu nhiên; hãy lấy khóa bằng script quản trị trước khi mở ngrok công khai.

Đọc khóa dashboard trên terminal riêng rồi nhập vào cửa sổ Cài đặt kết nối (installer sinh khóa ngẫu nhiên nếu chưa có khóa cũ):

```bash
~/ai-camera/.venv/bin/python ~/ai-camera-release/scripts/dashboard_key.py --show
```

Mở dashboard, **Ctrl+F5**, nhập khóa. Khi dùng camera iPhone, mở qua link ngrok **HTTPS**, cấp quyền camera rồi chọn camera trước/sau.

## Tự khởi động local và ngrok

Installer bật `ai-camera.service` khi Pi khởi động. Nếu đã cài ngrok ở `/usr/bin/ngrok` hoặc `~/ngrok-demo/ngrok`, installer cũng tạo `ai-camera-ngrok.service`; token được đọc từ cấu hình ngrok hiện có, không nằm trong ZIP. Xem link public sau khi Pi lên:

```bash
curl http://127.0.0.1:4040/api/tunnels
sudo systemctl status ai-camera ai-camera-ngrok --no-pager
```

Nếu chưa muốn public dashboard, tắt tunnel:

```bash
sudo systemctl disable --now ai-camera-ngrok
```

## LAN hostname và fallback ESP32

### Hostname cần dùng khi mất Internet

Khi mất Internet nhưng ESP32/điện thoại và Pi vẫn ở cùng Wi-Fi/LAN, dùng đúng
hostname:

```text
ai-camera.local
```

Địa chỉ endpoint LAN là:

```text
http://ai-camera.local:8000
```

Không dùng IP `192.168.0.94` và không dùng link ngrok trong chế độ này. Ngrok cần
Internet; hostname mDNS chỉ hoạt động khi thiết bị và Pi cùng mạng nội bộ.

Pi phải có hostname mDNS `ai-camera.local`; địa chỉ IP có thể thay đổi theo router
mà không cần sửa firmware:

```bash
sudo apt-get install -y avahi-daemon
sudo hostnamectl set-hostname ai-camera
sudo systemctl enable --now avahi-daemon
getent hosts ai-camera.local
```

Đặt `LAN_API_KEY` trong `.env` trên Pi và chép cùng giá trị vào
`firmware/HybridTerminal/secrets.h` dưới macro `LAN_API_KEY_VALUE`. Nếu để trống trên Pi,
server dùng lại `API_KEY`. ESP32 ưu tiên Supabase; khi Cloud lỗi, nó gửi JPEG,
trạng thái, ACK và nhận kết quả/audio qua `http://ai-camera.local:8000`. Dữ liệu
được lưu vào `data/hybrid/jobs.sqlite3`, sau đó worker upload lại ảnh, audio và
kết quả khi Cloud hồi phục.

## SQL đã bổ sung

Bạn đã chạy `003_cloud_only.sql` và `004_worker_version.sql` trong phiên làm việc này. File nằm trong `migrations/` để quản lý phiên bản; không cần chạy lại khi cập nhật trên cùng project đã áp dụng.

003 bổ sung input nhiều khung hình, RPC gửi request/trao đổi trạng thái, âm thanh dùng chung và lỗi vĩnh viễn. 004 tách claim v5/v6 để worker cũ đang chạy không nhận nhầm job mới. Installer dừng worker cũ trước khi chép source và chỉ chạy một service `ai-camera` sau cập nhật.

Với project mới cần schema gốc, SQL 002, 003, 004 theo thứ tự; các file migrations không thay key.

## Camera Auto 2 giây

Bản **single**: Auto đếm ngược 2 giây ngay trên thiết bị, sau đó chụp và gửi **đúng một ảnh**. Không gửi ảnh tại giây 0/1; không có vòng tự chụp/gửi lại khi chưa rõ. Có kết quả (kể cả chưa nhận diện) thì dừng và phát clip; bấm **Nhận diện tiếp** để bắt đầu lượt mới.

Đây là thời gian căn tay trước khi chụp, không phải bằng chứng tay đã giữ ổn định liên tục. Transport Cloud dùng `mode=manual, capture_mode=auto` cho một ảnh để tương thích SQL 003/004 hiện có; không cần migration hay đổi key. Upload và Manual vẫn gửi một ảnh. Hàng đợi cũ 3 khung vẫn được đọc để tránh làm hỏng request đang chờ.

Worker dọn dữ liệu trong task riêng, không bắt lượt nhận diện kế tiếp chờ dọn Storage. Yêu cầu từ trình duyệt không tạo/upload JPEG cho TFT; ESP32 vẫn nhận JPEG màn hình. Kết quả vẫn phải được ghi thành công vào Supabase trước khi trả về dashboard.

Chọn **Cộng tất cả tay** khi cần cộng hai tay để đếm 6–10 ngón. Mặc định chọn tay nổi bật, không tự cộng mọi người trong ảnh. Với ảnh nhiều người, dùng **Chọn tay trong ảnh** để chỉ vùng tay mục tiêu. Nắm tay 0 ngón vẫn thuộc nhóm chưa nhận diện vì bộ clip hiện tại chỉ có 1–10.

## Giao diện

Khi có mạng, dashboard và ESP32 ưu tiên Supabase Cloud. Khi Supabase/Internet mất
kết nối, Pi tự nhận diện cục bộ và ESP32 chuyển qua LAN hostname `ai-camera.local`.
`CLOUD_LOCAL_FALLBACK=1` là mặc định để giữ luồng offline; đặt `0` chỉ khi muốn
Cloud-only.
Kết quả được lưu trong `DATA_DIR/hybrid/jobs.sqlite3` và vẫn xem được qua dashboard.

### Chạy local không cần Supabase

Trên Windows, mở PowerShell tại thư mục `project` rồi chạy:

```powershell
.\run_local.ps1
```

Hoặc sao chép `.env.local.example` thành `.env` rồi chạy `python scripts\serve.py`.
Chế độ này đặt `LOCAL_ONLY=1`, nhận diện bằng MediaPipe trên máy, lưu SQLite trong
`data/hybrid/jobs.sqlite3` và không kết nối Supabase. Mở `http://127.0.0.1:8000`.

Chỉ còn hai nguồn camera: điện thoại/máy đang mở trang và ESP32 qua Cloud/LAN. Video điện thoại vẽ trong canvas trong khung, có `playsinline`, không dùng fullscreen video của iPhone.

Khung ảnh mặc định lớn hơn bản v5, có nút **Phóng to ảnh**, nút chạm 44 px trở lên, chữ gợi ý 14 px và chế độ sáng/tối. Màn nhỏ có thể cần cuộn; chế độ phóng to ưu tiên ảnh thay vì ép ảnh quá nhỏ. Thông số kỹ thuật vẫn ở mục mở rộng. JS được tách thành camera, upload, Cloud queue, âm thanh, thiết bị và sự kiện.

Camera điện thoại tạm dừng khi ẩn trang; khi quay lại bấm Bật camera/Nhận diện tiếp. Chính sách này tránh chụp một chuỗi ảnh stale khi iOS tự dừng camera ở nền.

Dashboard ESP32 hiển thị ảnh gửi gần nhất; **camera trực tiếp để căn tay chạy tại TFT**, không stream video qua Storage liên tục.

## Firmware ESP32

Dùng duy nhất `project/firmware/HybridTerminal`. Giữ `secrets.h` của thiết bị đang có; ZIP không chứa khóa. File `secrets.example.h` chỉ là mẫu trống.

### Camera live (TFT + dashboard)

Arduino IDE: core **esp32 3.0.x**, thư viện **LovyanGFX**, board *ESP32S3 Dev Module*,
`PSRAM: OPI PSRAM`, `Flash Size: 16MB`, `Partition: 16M Flash (3MB APP/9.9MB FATFS)`.
WiFi cấu hình ngay trên thiết bị (xem mục **Cấu hình WiFi** bên dưới); `WIFI_SSID_VALUE`/`WIFI_PASSWORD_VALUE` trong `secrets.h` chỉ là mạng mặc định, không bắt buộc.

Mặc định firmware dành cho **bo đã sửa** (`SHARED_CAM_TFT_BUS 0` trong `config.h`):
chân 8–12 của socket TFT được cắt khỏi ESP32 và nối dây lại như sau:

| TFT | Nối tới |
| --- | --- |
| SCK | GPIO1 |
| SDI/MOSI | GPIO2 (chung đường với LED đỏ trên bo) |
| DC/RS | GPIO46 (chân strapping, để trống khi nạp code) |
| CS | GND |
| RESET | EN |

Camera và TFT chạy đồng thời. Mỗi vòng loop lấy khung QVGA mới nhất, vẽ 1:1 lên TFT
và phát cùng khung đó ở `http://<ip-esp32>:81/stream`; Pi chuyển tiếp qua
`/api/esp32/live/stream` (đặt `ESP32_LIVE_HOST` nếu không dùng `esp32-01.local`).
Dashboard tự hiện live khi chọn nguồn camera ESP32.

Bo **chưa sửa**: đặt `SHARED_CAM_TFT_BUS 1`. Khi đó TFT dùng lại GPIO 8–12 chung với
camera, `live_camera.h` chạy luân phiên từng khung (bật/tắt chân xuất cảm biến qua SCCB),
chậm hơn nhiều.

Serial Monitor (115200): `b s + -` giả lập 4 nút, `i` in FPS và thời gian chụp/vẽ, `w` mở hotspot cấu hình WiFi.

### Cấu hình WiFi (hotspot + trang cấu hình)

Làm theo cách của Xiaozhi ESP32, không cần sửa code khi đổi mạng:

- **Khởi động:** thử lần lượt các mạng đã lưu (mạng lưu gần nhất trước, 12 giây mỗi mạng;
  mạng trong `secrets.h` thử sau cùng). Vào được thì màn chào hiện **IP** và `esp32-01.local`.
  Mở `http://<ip>/` trong cùng mạng để xem và đổi WiFi.
- **Chưa có WiFi / không vào được mạng nào:** thiết bị phát hotspot mở `AI-Camera-XXXX`
  (4 ký tự cuối MAC). Màn TFT có mã QR để vào hotspot. Điện thoại vào hotspot thì trang
  cấu hình tự bật; nếu không, mở `http://192.168.4.1/`. Chọn mạng, nhập mật khẩu, bấm **Lưu và kết nối**.
- **Mất WiFi khi đang chạy:** màn cảnh báo, tự kết nối lại; quá 30 giây thì bật hotspot song song
  (SELECT để bật ngay) và vẫn thử lại các mạng đã lưu. Khi có mạng: màn "Đã kết nối lại" hiện IP mới
  cạnh IP cũ trong 8 giây, hotspot tắt sau 20 giây.
- **Đổi WiFi chủ động:** giữ **BACK 3 giây** ở màn chào, hoặc giữ BACK khi cấp điện, hoặc gõ `w` trên Serial.

Lưu tối đa 5 mạng trong NVS (Preferences `wifi`); nạp lại firmware không mất danh sách.
Trang cấu hình không có mật khẩu bảo vệ: ai trong cùng mạng LAN hoặc vào được hotspot đều đổi được WiFi.

### Giao diện TFT và nút vật lý

Giao diện “Đêm xanh · thẻ kết quả”, chữ tiếng Việt có dấu:

- **Màn chào:** “Xin chào!” và phát lời chào qua loa. SELECT để vào live.
- **Live:** khung camera 320×240 bên trái; cột phải có thẻ **Kết quả** và **Ý nghĩa**;
  footer hiện chế độ, chức năng SELECT và âm lượng.
  - **AUTO:** giữ yên tay 2 giây (thanh đếm ngược dưới khung) rồi tự chụp và gửi Pi.
  - **MANUAL:** giơ tay cố định vào khung rồi nhấn SELECT để chụp.
- **Kết quả:** ảnh vừa chụp đứng yên, hiện số ngón và câu ý nghĩa, loa đọc câu đó;
  8 giây sau (`AUTO_NEXT_ROUND_MS`) tự quay lại live.

| Nút | GPIO | Chức năng |
| --- | --- | --- |
| BACK1 | 47 | Đổi AUTO / MANUAL |
| SELECT1 | 21 | Bắt đầu · chụp (MANUAL) · chụp ngay (AUTO) · live tiếp |
| DOWN1 | 3 | Tăng âm lượng |
| UP1 | 14 | Giảm âm lượng |

Font (Noto Sans, SIL OFL) và lời chào được nhúng vào firmware bằng
`python scripts/make_tft_assets.py`, tạo `fonts_vi.h` và `welcome_audio.h`.
Đổi file lời chào: `python scripts/make_tft_assets.py --skip-fonts --wav <file.wav>`
(WAV mono 16-bit, 8–48 kHz).

Camera OV3660 (theo bảng kết nối, `config.h`): `XCLK=15`, `SIOD=4`, `SIOC=5`,
`Y9..Y2 = 16, 17, 18, 12, 10, 8, 9, 11`, `VSYNC=6`, `HREF=7`, `PCLK=13`,
`PWDN=-1`, `RESET=-1`.
ILI9488, MAX98357A/loa và 4 nút đã có mapping theo schematic; vẫn cần thử
nghiệm phần cứng thực tế. INMP441 được để dự phòng; bài toán cử chỉ hiện không
thu mic/ASR.

Auto gửi một JPEG sau khi đếm ngược; không ghi LittleFS/NVS cho từng mẫu chưa xác nhận. Chỉ lưu UUID Manual đã nhận vào Cloud để phục hồi sau reboot, mã kết quả đã tiêu thụ và ACK của kết quả cuối. Loa tự phát tối đa một lần mỗi UUID; mất điện đúng lúc phát có thể làm câu bị thiếu, dùng Replay khi cần.

Firmware tái sử dụng TLS socket, gộp báo trạng thái/đọc lệnh trong một RPC, giảm polling khi rảnh. Khi đang thu chuỗi 2 giây, không thực hiện TLS trên task đó. Nếu không có khung camera thì timeout và trả trạng thái lỗi, không kẹt vòng Auto mãi. Năm nút thực tế là Power/Next/Volume+/Volume-/Manual; lệnh Serial tương ứng là `p/n/+/-/m`.

Với ESP32-S3 N16R8, chọn flash/PSRAM/partition theo tài liệu module. Kiểm tra OPI PSRAM với nhà cung cấp; nên dùng app partition đủ lớn. Các nhánh driver đã kiểm tra biên dịch không thay thế thử trên board thật.

## Xác thực và ngrok

API dashboard ảnh, kết quả, lịch sử và điều khiển yêu cầu `API_KEY`; endpoint ESP32 LAN dùng `LAN_API_KEY`. App từ chối khởi động khi mở ngoài loopback mà key rỗng. `/health` công khai chỉ trả trạng thái cơ bản; phiên bản và mã lỗi ở `/api/diagnostics` có xác thực.

Có thể thêm Basic Auth của ngrok bằng mẫu `project/examples/ngrok-traffic-policy.example.yml`. Tạo bản riêng, thay mật khẩu mẫu và dùng Traffic Policy của ngrok khi mở tunnel. Không dùng API key của Supabase làm mật khẩu đăng nhập web. Tham khảo [ngrok Basic Auth](https://ngrok.com/docs/gateway/traffic-policy/actions/basic-auth). Chính sách ngrok là cấu hình tùy chọn, chưa áp dụng vào tài khoản ngrok của bạn trong phiên này.

## Dọn dữ liệu Cloud

Mặc định worker dọn request **đã hoàn tất/ACK/lỗi** cùng ảnh và ảnh màn hình sau 7 ngày. Mẫu Auto không được xác nhận dọn sau 60 phút. Không xóa pending/processing hay clip giọng dùng chung. Thao tác xóa object dùng Storage API trước khi xóa hàng PostgreSQL; không xóa trực tiếp hàng storage bằng SQL.

Các biến tùy chỉnh trong cấu hình Pi:

```dotenv
CLOUD_RETENTION_ENABLED=1
CLOUD_RETENTION_DAYS=7
CLOUD_PROBE_RETENTION_MINUTES=60
MAX_CROP_RETRIES=4
```

WAV được dùng chung theo SHA-256 trong thư mục riêng của thiết bị; kết quả ESP32
offline còn tạo một JPEG màn hình tiếng Việt trong outbox để gửi lại khi Cloud hồi
phục. Ảnh hỏng/4xx được đánh dấu failed ngay; lỗi mạng/503 có retry và lease
fencing. Bộ đếm polling tăng thời gian chờ lúc rảnh, được đánh thức khi có request
web hoặc lệnh mới.

## Tự kiểm tra trên Pi

```bash
curl -s http://127.0.0.1:8000/health
cd ~/ai-camera-release
~/ai-camera/.venv/bin/python scripts/verify_cloud.py
~/ai-camera/.venv/bin/python scripts/diagnose_cloud.py
sudo journalctl -u ai-camera -n 40 --no-pager
```

`verify_cloud.py` thử ảnh mẫu thật qua các route Cloud: tải ảnh, camera Manual, Auto một ảnh, WAV và ảnh lỗi. Kết quả ghi `data/validation/verify-cloud.json`. Camera trong script là dữ liệu ảnh mẫu mô phỏng, không chứng minh iPhone/ESP vật lý hoạt động.

Tests có trong source:

```bash
~/ai-camera/.venv/bin/python -m unittest discover -s tests/v6 -v
~/ai-camera/.venv/bin/python -m unittest discover -s tests -p test_counter.py -v
```

JS: Node 22, `npm ci --ignore-scripts`, `npm test`. Có khóa phiên bản Prettier, cấu hình Ruff và workflow CI. Bài kiểm tra camera xác nhận đợi 2 giây tại thiết bị rồi gửi đúng một ảnh, kể cả khi Cloud cần thêm 3 giây. Xem `VALIDATION.md` cho kết quả thực chạy và giới hạn.

Kiểm thử trình duyệt tự động tùy chọn: cài `requirements-dev.txt`, mở một Chrome riêng với `--headless=new --remote-debugging-port=9237 --remote-allow-origins=* --autoplay-policy=no-user-gesture-required --user-data-dir=/tmp/ai-camera-qa about:blank`, rồi chạy `python tests/browser_cloud.py --cdp http://127.0.0.1:9237 --url http://127.0.0.1:8000/`. Không mở cổng debug ra Internet và đóng Chrome thử nghiệm sau khi xong. Script đọc API_KEY dashboard từ `.env`, kiểm tra upload/Auto/Manual, phát một lần, giao diện desktop/mobile và lưu ảnh chụp cùng báo cáo trong `data/browser-qa/`. Camera ở test này là video mô phỏng từ ảnh mẫu.

## Khôi phục

```bash
sudo systemctl stop ai-camera
~/ai-camera/.venv/bin/python ~/ai-camera-cloud-v6/install.py --rollback /home/lethinh/ai-camera-release/backups/cloud-v6-THOI_GIAN
sudo systemctl start ai-camera
```

Dùng đúng đường dẫn backup installer đã in. Rollback source không xóa dữ liệu Cloud và không đổi các key.
