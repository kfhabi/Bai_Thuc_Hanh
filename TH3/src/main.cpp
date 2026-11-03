#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include "th3_core.h"

// Arduino-ESP32 2.0.17; APIs here deliberately match the pinned PlatformIO platform.
namespace {
constexpr uint8_t LED_PIN = 26, START_PIN = 27, STOP_PIN = 25;
constexpr uint8_t PWM_CHANNEL = 0, PWM_BITS = 8;
constexpr uint32_t PWM_HZ = 1000, DEBOUNCE_MS = 35, HOLD_MS = 3000;
constexpr uint32_t TIMER_TICKS = 100000, PERIOD_US = 100000;
constexpr uint32_t CONNECT_MS = 15000, PORTAL_MS = 120000;
const char *AP_NAME = "TH3-Setup";
const char *AP_PASSWORD = "th3setup1"; // Public demonstration password, not a private credential.

th3::Controller controller(5000);
hw_timer_t *hardwareTimer = nullptr;
portMUX_TYPE isrMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t timerTicks = 0, startEdges = 0, stopEdges = 0;
uint32_t seenTicks = 0, seenStartEdges = 0, seenStopEdges = 0;
uint32_t timerHandledUs = 0, millisHandledUs = 0, lastMillisTick = 0;
uint32_t acceptedStart = 0, acceptedStop = 0;
bool inputValid = true, stressLoad = false, timerHealthy = true;
bool requestStart = false, requestStop = false, requestReset = false;
bool longHandled = false;
uint8_t selectedDuty = 128;
int appliedDuty = -1;
String serialLine;
Preferences preferences;
bool preferencesReady = false;
WiFiServer web(80);
WiFiClient httpClient;
String httpRequest, httpResponse;
uint32_t httpSince = 0;
int httpHeaderEnd = -1, httpBodyLength = 0;
size_t httpSent = 0;
bool connectAfterReply = false;
bool portalActive = false, connecting = false, wifiFailed = false, credentialsConfigured = false;
bool queueConnect = false, hasCandidate = false;
uint32_t portalSince = 0, connectSince = 0;
String candidateSsid, candidatePassword;

void serviceHttp(uint32_t now);
void closeHttp(bool completed = false) {
  httpClient.stop();
  httpRequest = ""; httpResponse = "";
  httpHeaderEnd = -1; httpBodyLength = 0; httpSent = 0;
  if (connectAfterReply) {
    if (completed) queueConnect = true;
    else { hasCandidate = false; candidateSsid = ""; candidatePassword = ""; }
  }
  connectAfterReply = false;
}

struct Button {
  bool raw = false, stable = false, pressed = false, released = false;
  uint32_t changedAt = 0, pressedAt = 0;
  void update(uint32_t now, bool down) {
    pressed = released = false;
    if (down != raw) { raw = down; changedAt = now; }
    if (stable != raw && uint32_t(now - changedAt) >= DEBOUNCE_MS) {
      stable = raw;
      if (stable) { pressed = true; pressedAt = now; }
      else released = true;
    }
  }
} startButton, stopButton;

void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&isrMux);
  ++timerTicks;
  portEXIT_CRITICAL_ISR(&isrMux);
}
void IRAM_ATTR onStartEdge() {
  portENTER_CRITICAL_ISR(&isrMux);
  ++startEdges;
  portEXIT_CRITICAL_ISR(&isrMux);
}
void IRAM_ATTR onStopEdge() {
  portENTER_CRITICAL_ISR(&isrMux);
  ++stopEdges;
  portEXIT_CRITICAL_ISR(&isrMux);
}

void event(const char *type, String detail) {
  detail.replace("\"", "\"\"");
  detail.replace("\r", " "); detail.replace("\n", " ");
  Serial.printf("%lu,%s,\"%s\"\n", (unsigned long)millis(), type, detail.c_str());
}

void outputOff() {
  ledcWrite(PWM_CHANNEL, 0);
  if (appliedDuty != 0) { appliedDuty = 0; event("output", "duty=0"); }
}

bool controlReady() { return timerHealthy && !connecting && !portalActive && !wifiFailed; }

void serviceControl(uint32_t now) {
  const th3::State before = controller.state();
  controller.update(now, requestStart, requestStop || digitalRead(STOP_PIN) == LOW, inputValid && timerHealthy,
                    requestReset, controlReady(), portalActive || connecting);
  if (requestStart && !controller.running()) event("start_rejected", controller.reason());
  requestStart = requestStop = requestReset = false;
  const uint8_t duty = controller.running() ? selectedDuty : 0;
  if (appliedDuty != duty) {
    ledcWrite(PWM_CHANNEL, duty); appliedDuty = duty;
    event("output", "duty=" + String(duty));
  }
  if (before != controller.state()) {
    event("state", String(th3::stateName(before)) + "->" + th3::stateName(controller.state()) +
          " reason=" + controller.reason());
  }
}

void status() {
  uint32_t ticks, startRaw, stopRaw;
  portENTER_CRITICAL(&isrMux);
  ticks = timerTicks; startRaw = startEdges; stopRaw = stopEdges;
  portEXIT_CRITICAL(&isrMux);
  event("status", String("state=") + th3::stateName(controller.state()) +
    " reason=" + controller.reason() + " valid=" + String(inputValid) +
    " duty=" + String(appliedDuty) + " selected=" + String(selectedDuty) +
    " timer_ticks=" + String(ticks) + " start_falling_edges=" + String(startRaw) +
    " start_debounced_presses=" + String(acceptedStart) +
    " stop_falling_edges=" + String(stopRaw) +
    " stop_debounced_presses=" + String(acceptedStop) +
    " portal=" + String(portalActive) + " connecting=" + String(connecting) +
    " wifi_failed=" + String(wifiFailed) + " load=" + String(stressLoad));
}

void cancelWifi(const char *reason) {
  outputOff(); requestStop = true;
  queueConnect = false;
  if (connecting) {
    WiFi.disconnect(false, false);
    wifiFailed = true; // A cancelled/failed attempt never silently permits START.
  }
  connecting = false; hasCandidate = false;
  candidateSsid = ""; candidatePassword = "";
  if (portalActive) { closeHttp(); web.stop(); WiFi.softAPdisconnect(true); }
  portalActive = false;
  WiFi.mode(WIFI_STA);
  event("wifi", reason);
}

void beginPortal() {
  outputOff(); requestStop = true;
  serviceControl(millis());
  if (connecting) { WiFi.disconnect(false, false); wifiFailed = true; }
  connecting = false; queueConnect = false; hasCandidate = false;
  candidateSsid = ""; candidatePassword = "";
  if (portalActive) { closeHttp(); web.stop(); WiFi.softAPdisconnect(true); }
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(AP_NAME, AP_PASSWORD)) {
    wifiFailed = true; portalActive = false; event("wifi", "ap_start_failed"); return;
  }
  portalActive = true; portalSince = millis();
  web.begin();
  event("wifi", "portal_open ap=TH3-Setup address=http://192.168.4.1 timeout_ms=120000");
}

void beginConnection() {
  queueConnect = false;
  outputOff(); requestStop = true; serviceControl(millis());
  WiFi.disconnect(false, false);
  WiFi.mode(portalActive ? WIFI_AP_STA : WIFI_STA);
  connectSince = millis(); connecting = true;
  WiFi.begin(candidateSsid.c_str(), candidatePassword.c_str());
  // Never print SSID/password, request bodies, or credential contents.
  event("wifi", hasCandidate ? "candidate_connect_started timeout_ms=15000" :
                              "saved_connect_started timeout_ms=15000");
}

void serviceWifi(uint32_t now) {
  if (queueConnect) { beginConnection(); return; }
  now = millis();
  if (connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      bool saved = true;
      if (hasCandidate) {
        saved = preferencesReady && preferences.putString("ssid", candidateSsid) > 0 &&
                preferences.putString("password", candidatePassword) == candidatePassword.length();
        if (saved) credentialsConfigured = true;
      }
      connecting = false; hasCandidate = false;
      candidateSsid = ""; candidatePassword = "";
      wifiFailed = !saved;
      if (portalActive) { closeHttp(); web.stop(); WiFi.softAPdisconnect(true); portalActive = false; }
      WiFi.mode(WIFI_STA);
      requestStop = true;
      event("wifi", saved ? "connected credentials_saved_or_loaded output_stays_off" :
                            "connected_but_storage_failed output_stays_off");
    } else if (uint32_t(now - connectSince) >= CONNECT_MS) {
      WiFi.disconnect(false, false);
      connecting = false; wifiFailed = true; hasCandidate = false;
      candidateSsid = ""; candidatePassword = "";
      event("wifi", "connect_timeout start_locked retry_config_or_forget");
    }
  }
  if (portalActive && uint32_t(now - portalSince) >= PORTAL_MS) {
    cancelWifi("portal_timeout output_stays_off");
  }
  if (portalActive) serviceHttp(now);
  // Detect loss only after a successful/configured connection; no credentials means offline lab mode.
  if (!portalActive && !connecting && !wifiFailed && credentialsConfigured &&
      WiFi.status() != WL_CONNECTED) {
    outputOff(); requestStop = true; wifiFailed = true;
    event("wifi", "connection_lost start_locked retry_config_or_forget");
  }
}

void httpReply(int code, const String &body, const char *contentType = "text/plain; charset=utf-8") {
  httpResponse = "HTTP/1.1 " + String(code) + " Response\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: " +
    String(contentType) + "\r\nContent-Length: " + String(body.length()) + "\r\n\r\n" + body;
  httpSent = 0;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decodeForm(const String &encoded, String &decoded) {
  decoded = "";
  for (size_t i = 0; i < encoded.length(); ++i) {
    char ch = encoded[i];
    if (ch == '+') ch = ' ';
    else if (ch == '%') {
      if (i + 2 >= encoded.length()) return false;
      const int hi = hexDigit(encoded[i + 1]), lo = hexDigit(encoded[i + 2]);
      if (hi < 0 || lo < 0) return false;
      ch = char(hi * 16 + lo); i += 2;
    }
    if (!ch || ch == '\r' || ch == '\n') return false;
    decoded += ch;
  }
  return true;
}

void processHttp() {
  if (httpRequest.startsWith("GET / HTTP/1.")) {
    httpReply(200, F(
      "<!doctype html><html lang='vi'><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
      "<title>TH3 Wi-Fi</title><h1>Cau hinh Wi-Fi TH3</h1><p>LED dang o trang thai an toan.</p>"
      "<p>Nhap thong tin router Wi-Fi 2.4 GHz. Mat khau AP TH3-Setup chi dung de ket noi ESP32, khong nhap lai o day.</p>"
      "<form method='post' action='/save'><label>SSID router <input name='ssid' maxlength='32' required></label><br>"
      "<label>Mat khau router <input name='password' type='password' maxlength='63'></label><br>"
      "<button>Thu ket noi va luu</button></form><p>Thoat bang nut DUNG hoac lenh WIFI CANCEL.</p></html>"),
      "text/html; charset=utf-8");
  } else if (httpRequest.startsWith("POST /save HTTP/1.")) {
    if (connecting || queueConnect) { httpReply(409, "Connection in progress"); return; }
    const String body = httpRequest.substring(httpHeaderEnd + 4, httpHeaderEnd + 4 + httpBodyLength);
    String ssid, password;
    bool gotSsid = false, gotPassword = false;
    int fieldStart = 0;
    while (fieldStart <= int(body.length())) {
      int end = body.indexOf('&', fieldStart);
      if (end < 0) end = body.length();
      const String field = body.substring(fieldStart, end);
      const int equals = field.indexOf('=');
      if (equals > 0) {
        const String name = field.substring(0, equals), value = field.substring(equals + 1);
        if (name == "ssid") {
          if (gotSsid || !decodeForm(value, ssid)) { httpReply(400, "Invalid SSID field"); return; }
          gotSsid = true;
        } else if (name == "password") {
          if (gotPassword || !decodeForm(value, password)) { httpReply(400, "Invalid password field"); return; }
          gotPassword = true;
        }
      }
      fieldStart = end + 1;
    }
    if (!gotSsid || !gotPassword || !ssid.length() || ssid.length() > 32 || password.length() > 63) {
      httpReply(400, "SSID/password length invalid"); return;
    }
    candidateSsid = ssid; candidatePassword = password;
    hasCandidate = true; connectAfterReply = true;
    outputOff(); requestStop = true;
    httpReply(200,
      "Dang thu ket noi trong toi da 15 giay. Xem Serial de biet ket qua. LED van tat.");
  } else httpReply(404, "Not found");
}

// Cooperative HTTP: bounded memory, <=256 bytes read/write per loop, 2s whole-request deadline.
// Socket send/recv use MSG_DONTWAIT; there is no wait-for-client loop or readStringUntil.
void serviceHttp(uint32_t now) {
  if (httpClient.fd() < 0) {
    httpClient = web.available();
    if (httpClient.fd() < 0) return;
    httpSince = now; httpClient.setNoDelay(true);
  }
  if (uint32_t(now - httpSince) >= 2000) { closeHttp(); event("http", "request_timeout"); return; }
  if (httpResponse.length()) {
    const size_t remaining = httpResponse.length() - httpSent;
    const size_t chunk = remaining < 256 ? remaining : 256;
    const int sent = send(httpClient.fd(), httpResponse.c_str() + httpSent, chunk, MSG_DONTWAIT);
    if (sent > 0) httpSent += sent;
    else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { closeHttp(); return; }
    if (httpSent == httpResponse.length()) closeHttp(true);
    return;
  }
  char buffer[256];
  const int received = recv(httpClient.fd(), buffer, sizeof(buffer), MSG_DONTWAIT);
  if (received == 0) { closeHttp(); return; }
  if (received < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) closeHttp();
    return;
  }
  for (int i = 0; i < received; ++i) httpRequest += buffer[i];
  if (httpHeaderEnd < 0) {
    httpHeaderEnd = httpRequest.indexOf("\r\n\r\n");
    if ((httpHeaderEnd < 0 && httpRequest.length() > 2048) || httpHeaderEnd > 2048) {
      httpReply(413, "Headers too large"); return;
    }
    if (httpHeaderEnd >= 0) {
      String headers = httpRequest.substring(0, httpHeaderEnd);
      headers.toLowerCase();
      if (headers.indexOf("\r\ntransfer-encoding:") >= 0) { httpReply(400, "Unsupported transfer encoding"); return; }
      const int marker = headers.indexOf("\r\ncontent-length:");
      if (marker >= 0) {
        const int valueStart = marker + 17;
        int end = headers.indexOf("\r\n", valueStart);
        if (end < 0) end = headers.length();
        String value = headers.substring(valueStart, end); value.trim();
        if (!value.length() || value.length() > 3) { httpReply(413, "Body too large"); return; }
        for (size_t i = 0; i < value.length(); ++i) {
          if (value[i] < '0' || value[i] > '9') { httpReply(400, "Invalid length"); return; }
        }
        httpBodyLength = value.toInt();
      } else if (httpRequest.startsWith("POST ")) { httpReply(411, "Content-Length required"); return; }
      if (httpBodyLength > 512) { httpReply(413, "Body too large"); return; }
    }
  }
  if (httpHeaderEnd >= 0 && int(httpRequest.length()) >= httpHeaderEnd + 4 + httpBodyLength) processHttp();
}

void command(String line) {
  line.trim(); line.toUpperCase();
  if (line == "HELP") {
    event("help", "STATUS | START | STOP | RESET | FAULT ON | FAULT OFF | PWM 0|50|100 | LOAD ON|OFF | WIFI CONFIG|CANCEL|FORGET");
  } else if (line == "STATUS") status();
  else if (line == "START") requestStart = true;
  else if (line == "STOP") {
    outputOff(); requestStop = true;
    if (portalActive || connecting || queueConnect) cancelWifi("cancelled_by_stop");
  } else if (line == "RESET") requestReset = true;
  else if (line == "FAULT ON") {
    outputOff(); inputValid = false; event("input", "invalid simulated_data_fault");
  } else if (line == "FAULT OFF") {
    inputValid = true; event("input", "valid fault_reset_still_required");
  } else if (line == "PWM 0" || line == "PWM 50" || line == "PWM 100") {
    selectedDuty = line == "PWM 0" ? 0 : line == "PWM 50" ? 128 : 255;
    event("pwm", "selected_duty=" + String(selectedDuty) + " start_required_when_stopped");
  } else if (line == "LOAD ON" || line == "LOAD OFF") {
    stressLoad = line == "LOAD ON";
    event("load", stressLoad ? "on bounded_busy_work_us=3000_per_loop" : "off");
  } else if (line == "WIFI CONFIG") beginPortal();
  else if (line == "WIFI CANCEL") cancelWifi("cancelled_by_command");
  else if (line == "WIFI FORGET") {
    cancelWifi("forget_requested output_stays_off");
    WiFi.disconnect(false, true);
    if (preferencesReady && preferences.clear()) {
      credentialsConfigured = false;
      wifiFailed = false; event("wifi", "credentials_forgotten explicit_offline_lab_mode");
    } else { wifiFailed = true; event("wifi", "forget_storage_failed start_locked"); }
  } else event("command", "unknown use_HELP"); // Do not echo arbitrary input; it might contain secrets.
  serviceControl(millis());
}

void serviceSerial() {
  for (uint8_t n = 0; n < 64 && Serial.available(); ++n) {
    const char ch = char(Serial.read());
    if (ch == '\n' || ch == '\r') {
      if (serialLine.length()) { command(serialLine); serialLine = ""; }
    } else if (serialLine.length() < 80) serialLine += ch;
    else serialLine = "";
  }
}

void serviceButtons(uint32_t now) {
  uint32_t startRaw, stopRaw;
  portENTER_CRITICAL(&isrMux);
  startRaw = startEdges; stopRaw = stopEdges;
  portEXIT_CRITICAL(&isrMux);
  startButton.update(now, digitalRead(START_PIN) == LOW);
  stopButton.update(now, digitalRead(STOP_PIN) == LOW);
  // Raw STOP has priority before debounce; filtering may suppress duplicate actions, never delay safe output.
  if (stopRaw != seenStopEdges || digitalRead(STOP_PIN) == LOW) {
    outputOff(); requestStop = true;
    if (startButton.raw || startButton.stable) longHandled = true;
    if (portalActive || connecting || queueConnect) cancelWifi("cancelled_by_stop_button");
  }
  seenStartEdges = startRaw; seenStopEdges = stopRaw;
  if (startButton.pressed) { ++acceptedStart; longHandled = false; event("button", "start_debounced_press"); }
  if (stopButton.pressed) {
    ++acceptedStop; requestReset = true;
    event("button", "stop_debounced_press reset_if_input_valid");
  }
  if (startButton.stable && !longHandled && uint32_t(now - startButton.pressedAt) >= HOLD_MS) {
    longHandled = true; beginPortal();
  }
  if (startButton.released) {
    if (!longHandled) requestStart = true;
    longHandled = false;
  }
}

void serviceTimer(uint32_t now) {
  uint32_t ticks;
  portENTER_CRITICAL(&isrMux); ticks = timerTicks; portEXIT_CRITICAL(&isrMux);
  if (ticks != seenTicks) {
    const uint32_t observedUs = micros(), delta = ticks - seenTicks;
    event("timer", "ticks=" + String(ticks) + " pending=" + String(delta) +
      " loop_dt_us=" + String(uint32_t(observedUs - timerHandledUs)) +
      " nominal_tick_us=" + String(PERIOD_US) + " load=" + String(stressLoad));
    seenTicks = ticks; timerHandledUs = observedUs;
  }
  if (uint32_t(now - lastMillisTick) >= 100) {
    const uint32_t observedUs = micros();
    event("millis", "loop_dt_us=" + String(uint32_t(observedUs - millisHandledUs)) +
      " target_us=100000 load=" + String(stressLoad));
    lastMillisTick = now; millisHandledUs = observedUs;
  }
}
} // namespace

void setup() {
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
  ledcSetup(PWM_CHANNEL, PWM_HZ, PWM_BITS); ledcAttachPin(LED_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);
  pinMode(START_PIN, INPUT_PULLUP); pinMode(STOP_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  Serial.println("t_ms,type,detail");
  outputOff();
  event("boot", "TH03 individual kit; synthetic input; STOPPED; use HELP; no hardware test claimed");
  attachInterrupt(digitalPinToInterrupt(START_PIN), onStartEdge, FALLING);
  attachInterrupt(digitalPinToInterrupt(STOP_PIN), onStopEdge, FALLING);
  hardwareTimer = timerBegin(0, 80, true);
  if (hardwareTimer) {
    timerAttachInterrupt(hardwareTimer, &onTimer, true);
    timerAlarmWrite(hardwareTimer, TIMER_TICKS, true);
    timerAlarmEnable(hardwareTimer);
    event("timer_config", "apb_hz=" + String(getApbFrequency()) +
      " divider=80 alarm_ticks=100000 nominal_us=100000 timestamp_source=loop_not_ISR");
  } else { timerHealthy = false; event("timer_config", "failed start_locked"); }
  timerHandledUs = millisHandledUs = micros(); lastMillisTick = millis();
  preferencesReady = preferences.begin("th3wifi", false);
  WiFi.persistent(false); WiFi.setAutoReconnect(false); WiFi.mode(WIFI_STA);
  if (preferencesReady) {
    candidateSsid = preferences.getString("ssid", "");
    candidatePassword = preferences.getString("password", "");
    if (candidateSsid.length()) { credentialsConfigured = true; hasCandidate = false; queueConnect = true; }
    else event("wifi", "no_saved_credentials offline_lab_mode");
  } else { wifiFailed = true; event("wifi", "storage_unavailable start_locked"); }
  // A scheduled boot connection also gates START before the first loop iteration.
  if (queueConnect) beginConnection();
  serviceControl(millis()); status();
}

void loop() {
  serviceButtons(millis());
  serviceControl(millis());
  serviceSerial();
  serviceWifi(millis());
  serviceControl(millis());
  serviceTimer(millis());
  if (stressLoad) {
    const uint32_t started = micros();
    while (uint32_t(micros() - started) < 3000) { asm volatile("nop"); }
  }
}
