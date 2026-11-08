#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <math.h>
#include "secrets.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "th3_core.h"
#include <string.h>

DHT dht(14, DHT11);

WiFiClient espClient;
PubSubClient client(espClient);
struct SensorSample {
  uint32_t seq;
  uint32_t uptimeMs;
  float temp;
  float hum;
  bool tempValid;
  bool humValid;
};

QueueHandle_t telemetryQueue = nullptr;
void publishQueuedSample();
enum class ControlCommand : uint8_t {
  Start, Stop, Reset
};

struct SafetyStatus {
  th3::State state;
  bool running;
  uint32_t uptimeMs;
  uint32_t checks;
  uint32_t maxGapMs;
  uint32_t lateCount;
  char reason[24];
};

QueueHandle_t commandQueue = nullptr;
QueueHandle_t latestSensorQueue = nullptr;
QueueHandle_t statusQueue = nullptr;

TaskHandle_t safetyTaskHandle = nullptr;
constexpr uint32_t STOP_BIT = 1u << 0;

// Khai báo chân GPIO
const int PIN_LED_RED = 26;
const int PIN_START = 27;
const int PIN_STOP = 25;

// Hàm xử lý khi bạn bấm nút hoặc gửi JSON từ Web Dashboard
void callback(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  if (!doc["cmd"].is<const char*>()) return;
  if (safetyTaskHandle == nullptr) return;

  const char* action = doc["cmd"];

  if (strcmp(action, "STOP") == 0) {
    xTaskNotify(safetyTaskHandle, STOP_BIT, eSetBits);
    return;
  }

  ControlCommand command;
  if (strcmp(action, "START") == 0) {
    command = ControlCommand::Start;
  } else if (strcmp(action, "RESET") == 0) {
    command = ControlCommand::Reset;
  } else {
    return;
  }

  if (xQueueSend(commandQueue, &command, 0) != pdPASS) {
    Serial.println("CMD rejected: queue full");
  }
}

void reconnect() {
  if (client.connect("sv23110186-node01",
                     MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("MQTT connected");
    client.subscribe("sv23110186/node01/cmd");
  } else {
    Serial.printf("MQTT connect failed: %d\n", client.state());
  }
}

void SensorTask(void* parameter) {
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t seq = 0;
  uint32_t dropped = 0;

  while (true) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(3000));

    SensorSample sample{};
    sample.seq = ++seq;
    sample.uptimeMs = millis();
    sample.temp = dht.readTemperature();
    sample.hum = dht.readHumidity();
    sample.tempValid = isfinite(sample.temp);
    sample.humValid = isfinite(sample.hum);

    xQueueOverwrite(latestSensorQueue, &sample);

    if (xQueueSend(telemetryQueue, &sample, 0) != pdPASS) {
      dropped++;
    }

    Serial.printf("SENSOR seq=%lu queue=%u dropped=%lu\n",
                  (unsigned long)sample.seq,
                  (unsigned)uxQueueMessagesWaiting(telemetryQueue),
                  (unsigned long)dropped);
  }
}

struct DebouncedPress {
  bool raw;
  bool stable;
  uint32_t changedMs;

  bool update(bool down, uint32_t now) {
    if (down != raw) {
      raw = down;
      changedMs = now;
    }

    if (raw != stable && uint32_t(now - changedMs) >= 35) {
      stable = raw;
      return stable;
    }

    return false;
  }
};

void SafetyTask(void* parameter) {
  th3::Controller controller(5000);

  const uint32_t beganMs = millis();
  uint32_t lastCheckMs = beganMs;
  uint32_t checks = 0, maxGapMs = 0, lateCount = 0;

  bool firstStart = digitalRead(PIN_START) == LOW;
  bool firstStop = digitalRead(PIN_STOP) == LOW;

  DebouncedPress startButton{firstStart, firstStart, beganMs};
  DebouncedPress stopButton{firstStop, firstStop, beganMs};

  TickType_t lastWake = xTaskGetTickCount();
  digitalWrite(PIN_LED_RED, LOW);

  while (true) {
    SensorSample sample{};
    bool haveSample =
        xQueuePeek(latestSensorQueue, &sample, 0) == pdPASS;

    uint32_t now = millis();
    uint32_t gap = now - lastCheckMs;
    lastCheckMs = now;

    if (gap > maxGapMs) maxGapMs = gap;
    if (checks != 0 && gap > 20) lateCount++;
    checks++;

    bool rawStop = digitalRead(PIN_STOP) == LOW;
    bool start =
        startButton.update(digitalRead(PIN_START) == LOW, now);
    bool stopEdge = stopButton.update(rawStop, now);
    bool stop = rawStop;
    bool reset = false;

    ControlCommand command;
    for (unsigned i = 0; i < 8; i++) {
      if (xQueueReceive(commandQueue, &command, 0) != pdPASS) {
        break;
      }

      if (command == ControlCommand::Start) start = true;
      if (command == ControlCommand::Stop) stop = true;
      if (command == ControlCommand::Reset) reset = true;
    }

    uint32_t notified = 0;
    xTaskNotifyWait(0, STOP_BIT, &notified, 0);
    if (notified & STOP_BIT) stop = true;

    if (stop) {
      start = false;
      xQueueReset(commandQueue);
    }

    if (stopEdge && controller.state() == th3::State::Fault) {
      reset = true;
    }

    bool freshValid =
        haveSample && sample.tempValid && sample.humValid &&
        uint32_t(now - sample.uptimeMs) <= 7000;

    bool grace =
        !haveSample && uint32_t(now - beganMs) < 7000;

    if (!freshValid) start = false;

    controller.update(now, start, stop, freshValid || grace,
                      reset, true, false);

    digitalWrite(PIN_LED_RED,
                 controller.running() ? HIGH : LOW);

    SafetyStatus status{};
    status.state = controller.state();
    status.running = controller.running();
    status.uptimeMs = now;
    status.checks = checks;
    status.maxGapMs = maxGapMs;
    status.lateCount = lateCount;

    snprintf(status.reason, sizeof(status.reason), "%s",
             controller.reason());

    xQueueOverwrite(statusQueue, &status);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));
  }
}

void MqttTask(void* parameter) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  espClient.setTimeout(2);
  client.setServer(MQTT_HOST, 1883);
  client.setBufferSize(512);
  client.setSocketTimeout(2);
  client.setCallback(callback);

  uint32_t lastAttempt = 0;

  const bool queueStressTest = false;
  uint32_t lastPublish = 0;

  Serial.printf("QUEUE_STRESS=%d\n", queueStressTest ? 1 : 0);  

  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (!client.connected()) {
      if (millis() - lastAttempt >= 2000) {
        reconnect();
        lastAttempt = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (client.loop()) {
      if (!queueStressTest ||
          uint32_t(millis() - lastPublish) >= 8000) {
        lastPublish = millis();
        publishQueuedSample();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_START, INPUT_PULLUP);
  pinMode(PIN_STOP, INPUT_PULLUP);

  dht.begin();
  telemetryQueue = xQueueCreate(10, sizeof(SensorSample));
  commandQueue = xQueueCreate(8, sizeof(ControlCommand));
  latestSensorQueue = xQueueCreate(1, sizeof(SensorSample));
  statusQueue = xQueueCreate(1, sizeof(SafetyStatus));  

  if (telemetryQueue == nullptr ||
    commandQueue == nullptr ||
    latestSensorQueue == nullptr ||
    statusQueue == nullptr)  {
    Serial.println("ERROR: Cannot create queue");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  digitalWrite(PIN_LED_RED, LOW);

  Serial.println("Queue ready: 10 samples");

  BaseType_t safetyCreated = xTaskCreatePinnedToCore(
    SafetyTask, "SafetyTask", 4096, nullptr,
    2, &safetyTaskHandle, 1);

  if (safetyCreated != pdPASS) {
    Serial.println("ERROR: Cannot create SafetyTask");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  BaseType_t sensorCreated = xTaskCreatePinnedToCore(
    SensorTask, "SensorTask", 4096, nullptr, 1, nullptr, 1);

  if (sensorCreated != pdPASS) {
    Serial.println("ERROR: Cannot create SensorTask");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  } 

  BaseType_t mqttCreated = xTaskCreatePinnedToCore(
    MqttTask, "MqttTask", 6144, nullptr, 1, nullptr, 0);

  if (mqttCreated != pdPASS) {
    Serial.println("ERROR: Cannot create MqttTask");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  } 
}

void publishQueuedSample() {

  // Định kỳ mỗi 2 giây gửi dữ liệu cảm biến & trạng thái lên Web
  SensorSample sample{};
  if (xQueuePeek(telemetryQueue, &sample, 0) == pdPASS) {

    JsonDocument doc;
    doc["device_id"] = "node01";
    doc["temp_unit"] = "C";
    doc["hum_unit"] = "%";
    doc["uptime_unit"] = "s";   
    // Giả lập số liệu (bạn có thể thay bằng dht.readTemperature() nếu có cảm biến DHT11/DHT22)
    float temp = sample.temp;
    float hum = sample.hum;
    bool tempValid = sample.tempValid;
    bool humValid = sample.humValid;    

    if (tempValid) doc["temp"] = temp;
    else doc["temp"] = nullptr;

    if (humValid) doc["hum"] = hum;
    else doc["hum"] = nullptr;

    doc["temp_valid"] = tempValid;
    doc["hum_valid"] = humValid;
    doc["rssi"] = WiFi.RSSI();
    doc["seq"] = sample.seq;
    doc["uptime"] = sample.uptimeMs / 1000;

    SafetyStatus safety{};
    if (xQueuePeek(statusQueue, &safety, 0) == pdPASS) {
      doc["ledR"] = safety.running ? 1 : 0;
      doc["state"] = th3::stateName(safety.state);
      doc["safety_checks"] = safety.checks;
    }

    char buffer[512];
    serializeJson(doc, buffer);
    if (client.publish("sv23110186/node01/telemetry", buffer)) {
      xQueueReceive(telemetryQueue, &sample, 0);
    } else {
      Serial.printf("MQTT publish_failed seq=%lu\n",
                    (unsigned long)sample.seq);
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
}

void loop() {
  SafetyStatus safety{};

  if (xQueuePeek(statusQueue, &safety, 0) == pdPASS) {
    Serial.printf(
        "SAFETY t_ms=%lu state=%s checks=%lu "
        "max_gap_ms=%lu late=%lu reason=%s\n",
        (unsigned long)safety.uptimeMs,
        th3::stateName(safety.state),
        (unsigned long)safety.checks,
        (unsigned long)safety.maxGapMs,
        (unsigned long)safety.lateCount,
        safety.reason);
  }

  vTaskDelay(pdMS_TO_TICKS(1000));
}