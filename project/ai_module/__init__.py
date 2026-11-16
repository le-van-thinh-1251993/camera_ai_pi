"""Module AI độc lập: Gemini nghĩ câu ý nghĩa cho số ngón tay + giọng đọc (Edge TTS).

Chạy riêng (service ai-camera-ai, cổng 127.0.0.1:8010). Core (app/) chỉ gọi qua HTTP
với hạn giờ; module tắt/lỗi thì core dùng lại câu cố định và file ghi âm.
"""
