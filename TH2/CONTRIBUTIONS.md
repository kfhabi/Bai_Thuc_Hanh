# Đóng góp cá nhân - TH02

- Họ tên: Đào Phương Linh
- Mã sinh viên: 23110186
- Hình thức: bài tập cá nhân
- Phương án: A - đo ngắn
- Thiết bị thực tế: ESP32 DevKit V1, DHT11 module 3 chân, biến trở, nút nhấn ngoài, LCD I²C 16×2

Bảng ghi công việc đã thực hiện trên phần cứng và dữ liệu đã thu tại nhà ngày 25/10/2026. 

| Công việc | Phần bản thân trực tiếp thực hiện | Kết quả/tệp minh chứng | Trạng thái |
|---|---|---|---|
| Nối mạch, xác định chân và nguồn | Trực tiếp nối DHT11 GPIO14, biến trở GPIO34, nút GPIO27 và LCD SDA21/SCL22; sửa lại kết nối biến trở để đọc được giá trị theo góc xoay | Bảng chân trong `README.md`; số đo và sự kiện trong `data/`; LCD VCC nối 5V/VIN đã được người thực hiện xác nhận | Đã chạy phần cứng |
| Đọc DHT11 và ghi CSV gốc | Trực tiếp nạp chương trình, chạy lệnh thu Serial và cung cấp dữ liệu đo thật | Hai phiên `d6716f6e-1` (`millis`) và `3ba6d063-1` (`delay`), mỗi phiên 40 mẫu hợp lệ | Đã ghi và kiểm tra dữ liệu |
| Thử lỗi và khôi phục cảm biến | Thực hiện lượt thử theo hướng dẫn ngắt/nối lại DATA DHT11 trong khi ghi dữ liệu | Phiên `5aae8f44-1`: 10 mẫu bình thường, 8 mẫu lỗi, 12 mẫu phục hồi; [mục thử lỗi](README.md#42-cách-báo-lỗi) | Đã có minh chứng thực tế |
| Đọc và quy đổi ADC | Trực tiếp xoay biến trở, sửa kết nối và quan sát giá trị thay đổi | Các cột `adc_raw`, `adc_percent`, `adc_mv` trong CSV; [mục ADC](README.md#41-cấu-hình-setup) | Đã có dữ liệu; công thức được hỗ trợ trong mã nguồn |
| Quét I²C và sử dụng LCD | Trực tiếp nối LCD và quan sát hiển thị số đo, thử nút đổi trang | Log quét có ACK `0x27`, SDA21/SCL22; LCD khởi tạo 16×2 và có sự kiện đổi trang | Đã giao tiếp trên thiết bị |
| So sánh chờ dài và mốc thời gian, thử nút | Trực tiếp nạp hai phiên bản và chạy hai lượt; xác nhận nhấn 6 lần ở bản `millis`; ở bản `delay` có nhấn nhưng không nhớ số lần | `millis`: khoảng cách mẫu 3000 ms, nhận 6 sự kiện nút; `delay`: 3046 ms, không ghi nhận nút; [mục lập lịch](README.md#44-kết-quả-thí-nghiệm-lập-lịch) | Đã đo; chưa đo độc lập độ trễ nhấn vật lý |
| MA5, biểu đồ và nhận xét | Cung cấp dữ liệu đo thật để xử lý; phần mã lọc, xuất hình và nhận xét được Codex hỗ trợ | Bốn PNG và các JSON thống kê trong `figures/`; [mục MA5 và biểu đồ](README.md#43-bộ-lọc-đã-chọn) | Đã xuất và kiểm tra; cần tự đọc, giải thích kết quả |
| Chuẩn bị tệp nộp | Cung cấp số liệu và thông tin thực tế để hoàn thiện bài; phần tổng hợp kết quả và sắp xếp tệp được hỗ trợ | [Bộ tệp trong repo](README.md#file-trong-repo-này) | Đã có bản tổng hợp; cần tự đọc lại trước khi nộp |

## Hỗ trợ đã sử dụng

- Codex soạn mã nguồn firmware/công cụ ghi và phân tích CSV, tạo biểu đồ, kiểm tra dữ liệu và dự thảo tài liệu kết quả. Không coi các phần được hỗ trợ này là mã do bản thân tự viết độc lập.
- Môi trường phát triển: PlatformIO với Arduino framework. Thư viện dùng trong dự án: Adafruit DHT sensor library, Adafruit Unified Sensor, hd44780, pyserial và matplotlib; phiên bản được khai báo trong `platformio.ini` và `scripts/requirements.txt`.
- Bản thân trực tiếp thao tác phần cứng và thực hiện các lượt thu dữ liệu đã nêu. Cần đọc lại mã và nhận xét để xác nhận phần mình giải thích được.

## Hạn chế và phần cần hỗ trợ

- Bản `delay` chưa có số lần nhấn vật lý chính xác, nên chỉ kết luận đã nhấn nhưng chương trình không ghi nhận sự kiện; chưa tính tỷ lệ bỏ lỡ hoặc độ trễ tính bằng ms.
- Chưa có phép đo điện áp tham chiếu để đánh giá độ chính xác tuyệt đối của ADC, hoặc cảm biến tham chiếu để đánh giá độ chính xác nhiệt độ/độ ẩm.

