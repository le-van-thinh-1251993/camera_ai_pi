# Đối chiếu bản review

| Mục review | Xử lý trong v6 |
|---|---|
| Xoay key | Hoãn đúng yêu cầu. Không đổi anon/publishable/secret/device key; loại private và key thật khỏi gói mới |
| Dashboard không xác thực | Bắt buộc API_KEY; giữ key hiện có, chỉ sinh khi rỗng; health tối giản; có mẫu ngrok Basic Auth |
| Phần cứng chưa được kiểm chứng | Giữ GPIO chưa xác minh ở -1; ghi rõ chưa thử vật lý. Có script thử Cloud trên Pi sau khi cài |
| Dữ liệu Cloud tăng vô hạn | Retention request/object, giọng WAV dùng chung, không tạo voice/screen trung gian |
| Quá nhiều request và độ trễ Auto | Một batch 3 ảnh/2 giây; polling backoff, pool HTTP Pi, TLS reuse ESP và RPC trao đổi trạng thái gộp |
| Đề xuất ưu tiên LAN | Không áp dụng vì yêu cầu mới của người dùng là tất cả nhận diện qua Supabase; route LAN cũ trả 410 |
| 4xx bị retry | Phân loại lỗi vĩnh viễn, live test ảnh hỏng failed sau một attempt |
| Flash/NVS Auto | Buffer RAM cho chuỗi ảnh; không persist mẫu trung gian; ACK chỉ cho kết quả cuối |
| Khóa LAN dùng chung | Luồng LAN nhận diện đã tắt; ESP dùng danh tính Supabase riêng với RLS |
| Bằng chứng tests không tái lập | Test JS/Python, script verify Cloud và CI có trong gói; ghi rõ mô phỏng/phần cứng |
| Khó bảo trì | Tách engine, protocol, web queue, worker, retention; JS chia module; Ruff/Prettier và CSS viết lại |
| Cấu hình/firmware cũ gây nhầm | Gói mới chỉ có HybridTerminal; dropdown chỉ một nguồn ESP32 Cloud |
| UX | Khung ảnh lớn hơn, phóng to, nút 44 px, chữ gợi ý 14 px, sáng/tối |
| victory_back chưa rõ quyền | Xác minh là ảnh người dùng cung cấp; không phát hành lại trong gói mới; tài liệu không gán giấy phép cho ảnh đó |
| README/SQL/validation trùng | Một bản tài liệu và migrations ở gốc gói; không nhân bản xuống project |
| Cộng tay nhiều người | Mặc định tay nổi bật; tùy chọn cộng hai tay và chọn vùng; bỏ mặc định CLOUD_COUNT_ALL=1 |
| 0 ngón chưa có nhãn | Giữ phạm vi 1–10 theo bộ clip hiện có; không tự thêm clip/ý nghĩa 0 ngón |

Chưa có tập ảnh gán nhãn 50–100 ảnh mỗi mức 1–10; không báo cáo độ chính xác phần trăm từ bộ mẫu nhỏ. Chưa có pinout nên thử nghiệm board/nguồn/PCB và iPhone thực vẫn cần thực hiện sau.
