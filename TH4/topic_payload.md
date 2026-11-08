# Topic và payload MQTT — TH04

Bài làm cá nhân, MSSV **23110186**. Thiết bị **node01** là ESP32 đọc nhiệt độ và độ ẩm từ DHT11. Đầu ra đang sử dụng là một LED đỏ phản ánh trạng thái của SafetyTask.

## 1. Kết nối và topic

Luồng dữ liệu: **ESP32 → Wi-Fi → broker Mosquitto chạy bằng Docker Compose → MQTT client nhận dữ liệu**.

| Thông số | Giá trị |
| --- | --- |
| Tiền tố topic | `sv23110186/node01` |
| MQTT client ID của ESP32 | `sv23110186-node01` |
| Giao thức | MQTT 3.1.1 qua TCP, cổng `1883` |
| Địa chỉ broker | Giá trị `MQTT_HOST` trong cấu hình riêng `firmware/include/secrets.h` |
| Xác thực | Tài khoản và mật khẩu MQTT; broker tắt kết nối ẩn danh |
| QoS telemetry | `0` |
| Retain telemetry | `false` |
| QoS đăng ký nhận lệnh của ESP32 | `0` |

| Topic | Bên publish | Bên subscribe | Nội dung |
| --- | --- | --- | --- |
| `sv23110186/node01/telemetry` | ESP32 | MQTT client trên máy quan sát | JSON số đo và trạng thái |
| `sv23110186/node01/cmd` | MQTT client điều khiển | ESP32 | JSON lệnh `START`, `STOP`, `RESET` |

Khi kết nối lại broker thành công, ESP32 đăng ký lại topic nhận lệnh. Thông tin Wi-Fi và mật khẩu MQTT được cấu hình riêng; repo chỉ chứa mẫu `firmware/include/secrets.example.h`.

## 2. Payload telemetry thực tế

Payload dưới đây lấy từ bản tin cuối trong `log_10min.txt`, được máy nhận ghi lúc **2026-11-08T20:14:35+0700**. JSON chỉ được xuống dòng lại để dễ đọc.

```json
{
  "device_id": "node01",
  "temp_unit": "C",
  "hum_unit": "%",
  "uptime_unit": "s",
  "temp": 29,
  "hum": 70,
  "temp_valid": true,
  "hum_valid": true,
  "rssi": -75,
  "seq": 208,
  "uptime": 624,
  "ledR": 0,
  "state": "STOPPED",
  "safety_checks": 31202
}
```

| Trường | Kiểu JSON | Ý nghĩa |
| --- | --- | --- |
| `device_id` | string | Mã thiết bị, hiện là `node01`. |
| `temp_unit` | string | Đơn vị nhiệt độ: `C`, tương ứng độ C. |
| `hum_unit` | string | Đơn vị độ ẩm tương đối: `%`. |
| `uptime_unit` | string | Đơn vị của `uptime`: `s`, tương ứng giây. |
| `temp` | number hoặc null | Nhiệt độ DHT11 đọc được; `null` nếu số đo không hợp lệ. |
| `hum` | number hoặc null | Độ ẩm DHT11 đọc được; `null` nếu số đo không hợp lệ. |
| `temp_valid` | boolean | `true` khi kết quả nhiệt độ là số hữu hạn; `false` khi đọc lỗi. |
| `hum_valid` | boolean | `true` khi kết quả độ ẩm là số hữu hạn; `false` khi đọc lỗi. |
| `rssi` | integer | Cường độ tín hiệu Wi-Fi tại lúc publish, đơn vị dBm. |
| `seq` | integer | Số thứ tự lượt lấy mẫu, bắt đầu từ `1` sau khi ESP32 khởi động và tăng ở mỗi lượt, kể cả khi mẫu bị bỏ do queue đầy. |
| `uptime` | integer | Thời gian tương đối tại lúc lấy mẫu: số giây từ khi ESP32 khởi động. |
| `ledR` | integer | Trạng thái LED đỏ: `1` khi bộ điều khiển đang `RUNNING`, `0` khi không chạy. |
| `state` | string | Trạng thái FSM: `STOPPED`, `RUNNING` hoặc `FAULT`. |
| `safety_checks` | integer | Bộ đếm số lượt kiểm tra của SafetyTask; tăng trong quá trình task hoạt động và khởi tạo lại khi ESP32 khởi động. |

### Thời gian của bản tin

- `uptime` thuộc về mẫu cảm biến và **không phải Unix timestamp**.
- Dấu thời gian ở đầu mỗi dòng log do máy nhận thêm vào bằng định dạng `%I` của `mosquitto_sub`; nó nằm ngoài JSON.
- `temp`, `hum`, các cờ hợp lệ, `seq` và `uptime` lấy từ mẫu trong queue. `rssi`, `ledR`, `state` và `safety_checks` được lấy khi chuẩn bị publish. Khi có mẫu tồn trong queue, trạng thái an toàn có thể mới hơn số đo cảm biến.
- `ledR` phản ánh đầu ra LED do bộ điều khiển đặt, không phải phép đo trạng thái hoạt động thực tế của động cơ bơm.

### Phân biệt số đo lỗi và giá trị 0

Firmware kiểm tra riêng từng số đo bằng `isfinite()`. Số đo lỗi được gửi dưới dạng JSON `null`, đi cùng cờ hợp lệ `false`. Giá trị `0` vẫn được giữ nếu kết quả đọc là số hữu hạn.

Ví dụ dưới đây chỉ trích các trường liên quan khi nhiệt độ đọc lỗi nhưng độ ẩm hợp lệ:

```json
{
  "temp": null,
  "temp_valid": false,
  "hum": 70,
  "hum_valid": true
}
```

## 3. Payload điều khiển

Gửi JSON đến topic **`sv23110186/node01/cmd`**. Giá trị `cmd` phân biệt chữ hoa/chữ thường.

| Payload | Xử lý |
| --- | --- |
| `{"cmd":"START"}` | Yêu cầu chạy. SafetyTask chỉ cho phép khởi động khi đang dừng, dữ liệu cảm biến hợp lệ và còn mới, đồng thời không có yêu cầu dừng. Khi chạy đủ `5000 ms`, bộ điều khiển tự dừng do timeout. |
| `{"cmd":"STOP"}` | Yêu cầu dừng được callback chuyển bằng task notification đến SafetyTask. Khi xử lý STOP, SafetyTask ưu tiên dừng và xóa các lệnh đang chờ trong command queue. |
| `{"cmd":"RESET"}` | Yêu cầu thoát `FAULT` khi đầu vào đã hợp lệ trở lại. Sau reset, FSM về `STOPPED`; cần lệnh START mới để chạy. |

START và RESET đi qua command queue có sức chứa **8 lệnh**. Nếu queue đầy, lệnh mới bị từ chối và Serial ghi `CMD rejected: queue full`. STOP đi qua task notification nên không phụ thuộc vào chỗ trống của command queue.

JSON không hợp lệ, thiếu `cmd`, `cmd` không phải chuỗi hoặc giá trị lệnh không được hỗ trợ sẽ bị bỏ qua. MQTT client điều khiển nên gửi lệnh với **retain = false** để tránh lưu lệnh cũ trên broker.

Firmware hiện không có topic ACK riêng. Có thể quan sát kết quả qua `state` và `ledR` trong telemetry tiếp theo. Khi đầu vào cảm biến không hợp lệ hoặc quá cũ trên `7000 ms`, SafetyTask chuyển sang `FAULT` và tắt LED.

## 4. Chu kỳ gửi và xử lý mất dữ liệu

- SensorTask lấy mẫu định kỳ **3000 ms**.
- Bản firmware đóng gói đang đặt **`queueStressTest = false`**. MqttTask gửi các mẫu đang chờ khi Wi-Fi và MQTT sẵn sàng; trong điều kiện bình thường phía nhận thấy khoảng một bản tin mỗi 3 giây. Khi kết nối phục hồi, các mẫu tồn có thể được gửi dồn.
- Telemetry queue chứa tối đa **10 mẫu**, theo thứ tự FIFO. Khi đầy, firmware **bỏ mẫu mới**, giữ những mẫu đang chờ và tăng bộ đếm `dropped` trong Serial log.
- Nếu bật `queueStressTest = true` để thử tải, MqttTask chỉ thử gửi một mẫu sau mỗi **8000 ms**, làm bên gửi chậm hơn bên lấy mẫu.
- MqttTask xem mẫu đầu queue bằng `xQueuePeek()` và chỉ lấy mẫu khỏi queue sau khi `client.publish()` trả về thành công. Nếu publish thất bại, mẫu vẫn được giữ để thử lại.
- QoS 0 không bảo đảm máy subscribe đã nhận được bản tin chỉ từ kết quả `publish()`. Vì vậy phải đối chiếu `seq` trong log phía nhận. Queue nằm trong RAM và các bộ đếm khởi tạo lại khi ESP32 khởi động.

### Đối chiếu log 10 phút đã thu

| Chỉ số | Kết quả trong `log_10min.txt` |
| --- | --- |
| Dòng đầu | `2026-11-08T20:04:31+0700` |
| Dòng cuối | `2026-11-08T20:14:35+0700` |
| Khoảng thời gian nhận được ghi lại | **604 giây — 10 phút 4 giây** |
| Số bản tin JSON hợp lệ | **203** |
| Dãy số thứ tự | `seq=1` đến `seq=208` |
| Mẫu không có trong log | **5 mẫu: `seq=7–11`** |
| Thời điểm có khoảng thiếu mẫu | Từ `20:04:31` (`seq=6`) đến `20:04:47` (`seq=12`), cách nhau **16 giây** |
| Sau khoảng thiếu mẫu | `seq=12–208` liên tiếp, không có số thứ tự bị thiếu thêm |
| Chất lượng số đo | Cả 203 bản tin đều có `temp_valid=true` và `hum_valid=true` |

Log này ghi nhận rõ 5 mẫu thiếu lúc đầu; không coi đây là lượt truyền không mất mẫu. File nhận không đủ để xác định riêng nguyên nhân mất các mẫu đó. Các thử nghiệm queue đầy và ngắt/nối lại broker được lưu riêng trong `log_queue_full.txt` và `log_disconnect.txt`.
