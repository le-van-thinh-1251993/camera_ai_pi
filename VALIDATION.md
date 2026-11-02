# Bản single — kiểm thử bổ sung 30/10/2026

- 4 JavaScript tests qua, gồm Auto đếm ngược 2 giây rồi gửi **một ảnh**, gọi submit một lần và dừng, không tự gửi lượt mới.
- Thử worker mới với MediaPipe thật + Supabase thật: một JPEG → kết quả Count_2 đã commit → endpoint dashboard trả kết quả cùng WAV, không tạo screen cho browser. Thời gian xử lý lượt thử trên Windows là 3,72 giây, không phải số đo trên Pi hay cam kết độ trễ mạng.
- Firmware ESP32-S3 biên dịch lại qua: 1.255.163 byte flash, 59.524 byte global RAM. Chưa thử board vật lý.
- Không cần SQL bổ sung, không đổi key. Task dọn dữ liệu tách khỏi task nhận diện.

Các báo cáo bên dưới và reports/cloud.json, browser.json, device-protocol.json là kiểm thử nền bản v6 ba khung trước thay đổi single. Báo cáo mới: reports/single-image.json. Chạy verify_cloud.py sau khi cập nhật Pi để kiểm tra cài đặt thực tế.

---

# Kiểm thử bản Cloud v6 — 30/10/2026

Đã chạy MediaPipe 1.1.1 + OpenCV 5.0.0 thật trên Windows Python 3.12.14, kết nối đúng Supabase của dự án sau SQL 003/004. Đây không phải kết quả chạy bản v6 trên Pi Python 3.13 hay phần cứng ESP32 thật.

| Kiểm tra | Kết quả |
|---|---|
| Python: 8 unit tests, kiểm tra cú pháp, Ruff | Qua |
| JavaScript: 4 tests gồm âm thanh một lần và Cloud chậm 3 giây | Qua |
| Upload qua Storage/queue/worker/kết quả Cloud | Qua |
| Auto 3 khung trong 2 giây; đổi cử chỉ giữa chuỗi không phát/không tạo screen | Qua |
| JPEG hỏng: failed sau lần xử lý đầu tiên | Qua |
| API cần xác thực, health tối giản, các route nhận diện LAN cũ trả 410 | Qua |
| Dọn request cũ và ảnh screen trong thư mục lease; giữ WAV dùng chung | Qua, kiểm tra object đã bị xóa |
| Mô phỏng tài khoản ESP32: submit, nhận WAV/screen, ACK và gửi lại cùng UUID | Qua |
| 5 ảnh nhiều chi tiết người dùng gửi: chọn tay nổi bật | 2, 1, 1, 2, 2 ngón; không cộng các người khác |
| Chrome thực + Supabase thực: upload, Auto, Manual, camera dừng, audio một lần, không lỗi JS | Qua |
| Installer: giữ key đã có, cấu hình GPIO, câu nói, recording; rollback | Qua |
| ESP32-S3 Arduino core 3.3.12; nhánh camera + ILI9488 + I2S bật | Biên dịch qua: 1.254.967 byte flash, 59.524 byte global RAM |

Đã kèm test, script kiểm tra trên Pi và workflow CI. Workflow chưa được chạy trên GitHub trong phiên này. `reports/` chứa kết quả đã làm sạch; không chứa key.

Sau khi cài trên Pi, chạy `~/ai-camera/.venv/bin/python ~/ai-camera-release/scripts/verify_cloud.py` để xác nhận môi trường Python 3.13 hiện có. Chưa đo độ chính xác bằng bộ dữ liệu có nhãn; không khẳng định tỉ lệ chính xác. iPhone thật, GPIO, TFT, camera và loa vật lý vẫn cần thử sau khi xác định board/pinout. Video camera trong thử nghiệm Chrome là nguồn mô phỏng từ ảnh mẫu.

Không xoay các key theo yêu cầu. Gói mới không chứa key thật; installer giữ `.env.cloud` và `secrets.h` hiện có.
