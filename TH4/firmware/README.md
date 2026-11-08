# Firmware TH04 — ESP32

Dự án PlatformIO dùng board `esp32doit-devkit-v1`, framework Arduino.

## Cấu hình và chạy

1. Mở thư mục `firmware/` bằng VS Code có extension PlatformIO.
2. Trong terminal PowerShell tại thư mục này, tạo cấu hình riêng:

   ```powershell
   Copy-Item .\include\secrets.example.h .\include\secrets.h
   ```

3. Mở `include/secrets.h`, điền tên/mật khẩu Wi-Fi, địa chỉ IP broker, tài khoản và mật khẩu MQTT. Broker dùng cổng `1883`.
4. Chọn **PlatformIO: Build**, sau đó **Upload** vào ESP32. Serial Monitor dùng baud `115200`.

`include/secrets.h` được bỏ qua bởi Git. Chỉ đưa file cấu hình mẫu lên repo.

## Mã nguồn

- `src/main.cpp`: đọc DHT11, gửi MQTT, các task/queue FreeRTOS và SafetyTask.
- `include/th3_core.h`: bộ điều khiển trạng thái an toàn.
- `platformio.ini`: board, framework và các thư viện cần cài.

Ba file trên được chép nguyên vẹn từ dự án TH4 đang dùng. Bản này đang đặt `queueStressTest = false` để gửi dữ liệu bình thường. DHT11 dùng GPIO 14, LED đỏ GPIO 26, nút START GPIO 27 và STOP GPIO 25.
