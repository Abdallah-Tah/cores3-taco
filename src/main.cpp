#include <Arduino.h>
#include <M5Unified.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <math.h>

#include "AppConfig.h"

namespace {

constexpr uint16_t BG = 0x0000;
constexpr uint16_t PANEL = 0x0841;
constexpr uint16_t PANEL_ACTIVE = 0x10E3;
constexpr uint16_t CYAN = 0x065F;
constexpr uint16_t CYAN_DIM = 0x0218;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t BLUE_WHITE = 0xDFFF;
constexpr uint16_t MUTED = 0x7BEF;
constexpr uint32_t STATUS_PUBLISH_INTERVAL_MS = 30000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 5000;

enum class Mood : uint8_t { Happy, Curious, Sleepy, Surprised, Grumpy };
enum class Screen : uint8_t { Face, Home, Status };

struct TouchGesture {
  bool tracking = false;
  int startX = 0;
  int startY = 0;
  int x = 0;
  int y = 0;
  uint32_t startedAt = 0;
};

M5Canvas canvas(&M5.Display);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
TouchGesture touch;
Mood mood = Mood::Happy;
Screen screen = Screen::Face;
uint32_t nextBlinkAt = 0;
uint32_t blinkStartedAt = 0;
uint32_t lastStatusPublishAt = 0;
uint32_t lastWiFiAttemptAt = 0;
uint32_t lastMqttAttemptAt = 0;
bool blinking = false;
bool discoveryPublished = false;
float gazeX = 0.0f;
float gazeY = 0.0f;
float targetGazeX = 0.0f;
float targetGazeY = 0.0f;
float touchGlow = 0.0f;
String notification;
uint32_t notificationUntil = 0;

float clampf(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}

bool networkConfigured() {
  return AppConfig::WIFI_SSID[0] != '\0' && AppConfig::MQTT_HOST[0] != '\0';
}

const char* moodName(Mood value) {
  switch (value) {
    case Mood::Happy: return "happy";
    case Mood::Curious: return "curious";
    case Mood::Sleepy: return "sleepy";
    case Mood::Surprised: return "surprised";
    case Mood::Grumpy: return "grumpy";
  }
  return "happy";
}

const char* screenName(Screen value) {
  switch (value) {
    case Screen::Face: return "face";
    case Screen::Home: return "home";
    case Screen::Status: return "status";
  }
  return "face";
}

bool parseMood(const String& value, Mood& result) {
  if (value == "happy") result = Mood::Happy;
  else if (value == "curious") result = Mood::Curious;
  else if (value == "sleepy") result = Mood::Sleepy;
  else if (value == "surprised") result = Mood::Surprised;
  else if (value == "grumpy") result = Mood::Grumpy;
  else return false;
  return true;
}

bool parseScreen(const String& value, Screen& result) {
  if (value == "face") result = Screen::Face;
  else if (value == "home") result = Screen::Home;
  else if (value == "status") result = Screen::Status;
  else return false;
  return true;
}

String topic(const char* suffix) {
  return String(AppConfig::TOPIC_PREFIX) + "/" + suffix;
}

void publish(const char* suffix, const String& payload, bool retained = false) {
  if (mqtt.connected()) mqtt.publish(topic(suffix).c_str(), payload.c_str(), retained);
}

void publishMood() { publish("mood/state", moodName(mood), true); }
void publishScreen() { publish("screen/state", screenName(screen), true); }

void setMood(Mood value, bool announce = true) {
  mood = value;
  touchGlow = 1.0f;
  if (announce) publishMood();
}

void nextMood() {
  setMood(static_cast<Mood>((static_cast<uint8_t>(mood) + 1) % 5));
}

void setScreen(Screen value, bool announce = true) {
  screen = value;
  if (announce) publishScreen();
}

void changeScreen(int direction) {
  int value = static_cast<int>(screen) + direction;
  if (value < 0) value = 2;
  if (value > 2) value = 0;
  setScreen(static_cast<Screen>(value));
}

void showNotification(const String& message) {
  notification = message.substring(0, 64);
  notificationUntil = millis() + 8000;
  setMood(Mood::Curious);
}

String deviceJson() {
  return String("\"device\":{\"identifiers\":[\"") + AppConfig::DEVICE_ID +
         "\"],\"name\":\"" + AppConfig::DEVICE_NAME +
          "\",\"manufacturer\":\"Taco Project\",\"model\":\"CoreS3\","
          "\"sw_version\":\"1.0.0-alpha.1\"}";
}

void publishDiscoveryEntity(const String& component, const String& objectId,
                            const String& config) {
  const String discoveryTopic = "homeassistant/" + component + "/" +
                                AppConfig::DEVICE_ID + "/" + objectId + "/config";
  mqtt.publish(discoveryTopic.c_str(), config.c_str(), true);
}

void publishDiscovery() {
  const String availability = String("\"availability_topic\":\"") +
                              topic("availability") + "\",";
  const String device = deviceJson();

  publishDiscoveryEntity(
      "sensor", "battery",
      String("{\"name\":\"Battery\",\"unique_id\":\"") + AppConfig::DEVICE_ID +
          "_battery\",\"state_topic\":\"" + topic("status") +
          "\",\"value_template\":\"{{ value_json.battery }}\","
          "\"unit_of_measurement\":\"%\",\"device_class\":\"battery\","
          "\"state_class\":\"measurement\"," + availability + device + "}");

  publishDiscoveryEntity(
      "sensor", "wifi_signal",
      String("{\"name\":\"WiFi signal\",\"unique_id\":\"") + AppConfig::DEVICE_ID +
          "_wifi_signal\",\"state_topic\":\"" + topic("status") +
          "\",\"value_template\":\"{{ value_json.rssi }}\","
          "\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\","
          "\"state_class\":\"measurement\"," + availability + device + "}");

  publishDiscoveryEntity(
      "select", "mood",
      String("{\"name\":\"Mood\",\"unique_id\":\"") + AppConfig::DEVICE_ID +
          "_mood\",\"command_topic\":\"" + topic("mood/set") +
          "\",\"state_topic\":\"" + topic("mood/state") +
          "\",\"options\":[\"happy\",\"curious\",\"sleepy\",\"surprised\","
          "\"grumpy\"]," + availability + device + "}");

  publishDiscoveryEntity(
      "select", "screen",
      String("{\"name\":\"Screen\",\"unique_id\":\"") + AppConfig::DEVICE_ID +
          "_screen\",\"command_topic\":\"" + topic("screen/set") +
          "\",\"state_topic\":\"" + topic("screen/state") +
          "\",\"options\":[\"face\",\"home\",\"status\"]," + availability +
          device + "}");

  publishDiscoveryEntity(
      "text", "message",
      String("{\"name\":\"Message\",\"unique_id\":\"") + AppConfig::DEVICE_ID +
          "_message\",\"command_topic\":\"" + topic("message/set") +
          "\",\"state_topic\":\"" + topic("message/state") +
          "\",\"mode\":\"text\"," + availability + device + "}");
  discoveryPublished = true;
}

void publishStatus() {
  const int battery = M5.Power.getBatteryLevel();
  const int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100;
  const String payload = String("{\"battery\":") + battery +
                         ",\"rssi\":" + rssi +
                         ",\"uptime\":" + millis() / 1000 + "}";
  publish("status", payload, true);
  lastStatusPublishAt = millis();
}

void onMqttMessage(char* rawTopic, byte* payload, unsigned int length) {
  String value;
  value.reserve(length);
  for (unsigned int i = 0; i < length; ++i) value += static_cast<char>(payload[i]);
  value.trim();
  const String incoming(rawTopic);
  if (incoming == topic("mood/set")) {
    Mood requested;
    if (parseMood(value, requested)) setMood(requested);
  } else if (incoming == topic("screen/set")) {
    Screen requested;
    if (parseScreen(value, requested)) setScreen(requested);
  } else if (incoming == topic("message/set")) {
    showNotification(value);
    publish("message/state", value, true);
  }
}

void serviceNetwork(uint32_t now) {
  if (!networkConfigured()) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWiFiAttemptAt >= WIFI_RETRY_INTERVAL_MS || lastWiFiAttemptAt == 0) {
      lastWiFiAttemptAt = now;
      WiFi.mode(WIFI_STA);
      WiFi.setHostname(AppConfig::DEVICE_ID);
      WiFi.begin(AppConfig::WIFI_SSID, AppConfig::WIFI_PASSWORD);
    }
    return;
  }

  if (!mqtt.connected()) {
    discoveryPublished = false;
    if (now - lastMqttAttemptAt < MQTT_RETRY_INTERVAL_MS && lastMqttAttemptAt != 0) return;
    lastMqttAttemptAt = now;
    const String clientId = String(AppConfig::DEVICE_ID) + "-" +
                            String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
    const bool connected = mqtt.connect(
        clientId.c_str(), AppConfig::MQTT_USERNAME, AppConfig::MQTT_PASSWORD,
        topic("availability").c_str(), 0, true, "offline");
    if (!connected) return;
    publish("availability", "online", true);
    mqtt.subscribe(topic("mood/set").c_str());
    mqtt.subscribe(topic("screen/set").c_str());
    mqtt.subscribe(topic("message/set").c_str());
    publishDiscovery();
    publishMood();
    publishScreen();
    publishStatus();
  }

  mqtt.loop();
  if (!discoveryPublished) publishDiscovery();
  if (now - lastStatusPublishAt >= STATUS_PUBLISH_INTERVAL_MS) publishStatus();
}

void drawEye(int centerX, int centerY, float open, float lookX, float lookY) {
  constexpr int eyeWidth = 72;
  const int eyeHeight = max(4, static_cast<int>(68.0f * open));
  const int y = centerY - eyeHeight / 2;
  canvas.fillRoundRect(centerX - eyeWidth / 2 - 4, y - 4, eyeWidth + 8,
                       eyeHeight + 8, min(22, eyeHeight / 2 + 4), CYAN_DIM);
  canvas.fillRoundRect(centerX - eyeWidth / 2, y, eyeWidth, eyeHeight,
                       min(18, eyeHeight / 2), BLUE_WHITE);
  if (eyeHeight <= 18) return;
  const int pupilX = centerX + static_cast<int>(lookX * 11.0f);
  const int pupilY = centerY + static_cast<int>(lookY * 8.0f);
  canvas.fillRoundRect(pupilX - 10, pupilY - 14, 20, 28, 8, 0x18C3);
  canvas.fillCircle(pupilX - 3, pupilY - 5, 3, WHITE);
}

void drawBrows() {
  switch (mood) {
    case Mood::Grumpy:
      canvas.drawWideLine(66, 62, 132, 80, 7, CYAN);
      canvas.drawWideLine(188, 80, 254, 62, 7, CYAN);
      break;
    case Mood::Curious:
      canvas.drawWideLine(65, 70, 132, 63, 6, CYAN);
      canvas.drawWideLine(188, 69, 254, 74, 6, CYAN);
      break;
    case Mood::Surprised:
      canvas.drawArc(100, 60, 35, 29, 205, 335, CYAN);
      canvas.drawArc(220, 60, 35, 29, 205, 335, CYAN);
      break;
    default:
      canvas.drawWideLine(68, 70, 132, 66, 5, CYAN);
      canvas.drawWideLine(188, 66, 252, 70, 5, CYAN);
      break;
  }
}

void drawMouth() {
  switch (mood) {
    case Mood::Happy: canvas.drawArc(160, 168, 28, 23, 20, 160, CYAN); break;
    case Mood::Curious: canvas.fillCircle(160, 179, 7, CYAN); break;
    case Mood::Sleepy: canvas.drawWideLine(143, 180, 177, 180, 5, CYAN); break;
    case Mood::Surprised: canvas.drawCircle(160, 179, 13, CYAN); break;
    case Mood::Grumpy: canvas.drawArc(160, 196, 25, 18, 200, 340, CYAN); break;
  }
}

void drawPageDots() {
  for (int i = 0; i < 3; ++i) {
    const int x = 148 + i * 12;
    const bool active = i == static_cast<int>(screen);
    canvas.fillCircle(x, 229, active ? 3 : 2, active ? CYAN : CYAN_DIM);
  }
}

void drawNotification() {
  if (notification.isEmpty() || millis() >= notificationUntil) return;
  canvas.fillRoundRect(18, 182, 284, 36, 12, PANEL_ACTIVE);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(WHITE, PANEL_ACTIVE);
  canvas.setTextSize(1);
  canvas.drawString(notification, 160, 200);
}

void drawFace(uint32_t now) {
  canvas.fillScreen(BG);
  const float pulse = 0.5f + 0.5f * sinf(now * 0.003f);
  canvas.drawCircle(160, 118, 112, touchGlow > 0.05f ? WHITE : CYAN);
  canvas.drawCircle(160, 118, 110, CYAN_DIM);
  canvas.drawArc(160, 118, 117, 115, 210,
                 210 + static_cast<int>(pulse * 120), CYAN);

  float eyeOpen = mood == Mood::Sleepy ? 0.35f : 1.0f;
  if (blinking) {
    const float phase = (now - blinkStartedAt) / 220.0f;
    eyeOpen *= clampf(fabsf(phase * 2.0f - 1.0f), 0.04f, 1.0f);
    if (phase >= 1.0f) blinking = false;
  }
  if (mood == Mood::Surprised) eyeOpen = 1.15f;
  drawBrows();
  drawEye(105, 112, eyeOpen, gazeX, gazeY);
  drawEye(215, 112, eyeOpen, gazeX, gazeY);
  drawMouth();
  drawNotification();
  drawPageDots();
}

void drawHeader(const char* title) {
  canvas.setTextDatum(top_left);
  canvas.setTextSize(2);
  canvas.setTextColor(WHITE, BG);
  canvas.drawString(title, 16, 14);
  canvas.setTextSize(1);
  canvas.setTextColor(WiFi.status() == WL_CONNECTED ? CYAN : MUTED, BG);
  canvas.drawString(WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE", 252, 18);
}

void drawActionButton(int x, int y, const char* label, int index) {
  canvas.fillRoundRect(x, y, 136, 70, 14, PANEL);
  canvas.drawRoundRect(x, y, 136, 70, 14, CYAN_DIM);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(CYAN, PANEL);
  canvas.setTextSize(2);
  canvas.drawNumber(index, x + 68, y + 21);
  canvas.setTextSize(1);
  canvas.setTextColor(WHITE, PANEL);
  canvas.drawString(label, x + 68, y + 49);
}

void drawHome() {
  canvas.fillScreen(BG);
  drawHeader("HOME");
  drawActionButton(16, 54, AppConfig::ACTION_1, 1);
  drawActionButton(168, 54, AppConfig::ACTION_2, 2);
  drawActionButton(16, 136, AppConfig::ACTION_3, 3);
  drawActionButton(168, 136, AppConfig::ACTION_4, 4);
  drawPageDots();
}

void drawStatusRow(int y, const char* label, const String& value) {
  canvas.setTextDatum(middle_left);
  canvas.setTextSize(1);
  canvas.setTextColor(MUTED, BG);
  canvas.drawString(label, 22, y);
  canvas.setTextDatum(middle_right);
  canvas.setTextColor(WHITE, BG);
  canvas.drawString(value, 298, y);
  canvas.drawFastHLine(22, y + 17, 276, PANEL_ACTIVE);
}

void drawStatus() {
  canvas.fillScreen(BG);
  drawHeader("TACO");
  drawStatusRow(67, "Battery", String(M5.Power.getBatteryLevel()) + "%");
  drawStatusRow(104, "Wi-Fi",
                WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : "Not connected");
  drawStatusRow(141, "Home Assistant", mqtt.connected() ? "Connected" : "Not connected");
  drawStatusRow(178, "IP address",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "--");
  drawPageDots();
}

void drawCurrentScreen(uint32_t now) {
  switch (screen) {
    case Screen::Face: drawFace(now); break;
    case Screen::Home: drawHome(); break;
    case Screen::Status: drawStatus(); break;
  }
  canvas.pushSprite(0, 0);
}

void triggerHomeAction(int x, int y) {
  if (y < 54 || y > 206) return;
  const int column = x < 160 ? 0 : 1;
  const int row = y < 130 ? 0 : 1;
  const int action = row * 2 + column + 1;
  publish("action", String(action));
  showNotification(String("Sent action ") + action);
}

void handleTap(int x, int y) {
  if (screen == Screen::Face) nextMood();
  else if (screen == Screen::Home) triggerHomeAction(x, y);
}

void handleTouch() {
  const auto count = M5.Touch.getCount();
  if (!count) {
    targetGazeX *= 0.92f;
    targetGazeY *= 0.92f;
    return;
  }
  const auto detail = M5.Touch.getDetail(0);
  targetGazeX = clampf((detail.x - 160.0f) / 145.0f, -1.0f, 1.0f);
  targetGazeY = clampf((detail.y - 120.0f) / 105.0f, -1.0f, 1.0f);
  if (detail.wasPressed()) {
    touch.tracking = true;
    touch.startX = detail.x;
    touch.startY = detail.y;
    touch.x = detail.x;
    touch.y = detail.y;
    touch.startedAt = millis();
  }
  if (touch.tracking && detail.isPressed()) {
    touch.x = detail.x;
    touch.y = detail.y;
  }
  if (touch.tracking && detail.wasReleased()) {
    touch.tracking = false;
    const int deltaX = touch.x - touch.startX;
    const int deltaY = touch.y - touch.startY;
    if (abs(deltaX) > 65 && abs(deltaX) > abs(deltaY)) {
      changeScreen(deltaX < 0 ? 1 : -1);
    } else if (millis() - touch.startedAt < 700) {
      handleTap(touch.x, touch.y);
    }
  }
}

}  // namespace

void setup() {
  auto config = M5.config();
  config.clear_display = true;
  config.output_power = true;
  M5.begin(config);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(120);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(320, 240)) {
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
    M5.Display.drawString("Display buffer failed", 20, 110);
    while (true) delay(1000);
  }
  mqtt.setServer(AppConfig::MQTT_HOST, AppConfig::MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(1400);
  nextBlinkAt = millis() + 1800;
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  handleTouch();
  serviceNetwork(now);
  gazeX += (targetGazeX - gazeX) * 0.14f;
  gazeY += (targetGazeY - gazeY) * 0.14f;
  touchGlow *= 0.92f;
  if (!blinking && now >= nextBlinkAt && mood != Mood::Surprised) {
    blinking = true;
    blinkStartedAt = now;
    nextBlinkAt = now + 2200 + random(2600);
  }
  drawCurrentScreen(now);
  delay(16);
}
