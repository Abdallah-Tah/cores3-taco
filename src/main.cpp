#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <math.h>

#include "AppConfig.h"

extern const uint8_t faceHappyStart[] asm("_binary_assets_faces_happy_jpg_start");
extern const uint8_t faceHappyEnd[] asm("_binary_assets_faces_happy_jpg_end");
extern const uint8_t faceStartledStart[] asm("_binary_assets_faces_startled_jpg_start");
extern const uint8_t faceStartledEnd[] asm("_binary_assets_faces_startled_jpg_end");
extern const uint8_t faceCuriousStart[] asm("_binary_assets_faces_curious_jpg_start");
extern const uint8_t faceCuriousEnd[] asm("_binary_assets_faces_curious_jpg_end");
extern const uint8_t faceExcitedStart[] asm("_binary_assets_faces_excited_jpg_start");
extern const uint8_t faceExcitedEnd[] asm("_binary_assets_faces_excited_jpg_end");
extern const uint8_t faceSurprisedStart[] asm("_binary_assets_faces_surprised_jpg_start");
extern const uint8_t faceSurprisedEnd[] asm("_binary_assets_faces_surprised_jpg_end");
extern const uint8_t faceSleepyStart[] asm("_binary_assets_faces_sleepy_jpg_start");
extern const uint8_t faceSleepyEnd[] asm("_binary_assets_faces_sleepy_jpg_end");
extern const uint8_t faceSadStart[] asm("_binary_assets_faces_sad_jpg_start");
extern const uint8_t faceSadEnd[] asm("_binary_assets_faces_sad_jpg_end");
extern const uint8_t faceWinkStart[] asm("_binary_assets_faces_wink_jpg_start");
extern const uint8_t faceWinkEnd[] asm("_binary_assets_faces_wink_jpg_end");

namespace {

constexpr uint16_t BG = 0x0000;
constexpr uint16_t PANEL = 0x0841;
constexpr uint16_t PANEL_ACTIVE = 0x10E3;
constexpr uint16_t CYAN = 0x065F;
constexpr uint16_t CYAN_DIM = 0x0218;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t BLUE_WHITE = 0xDFFF;
constexpr uint16_t MUTED = 0x7BEF;
constexpr uint16_t PINK = 0xF9B7;
constexpr uint32_t STATUS_PUBLISH_INTERVAL_MS = 30000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t VOICE_SAMPLE_RATE = 24000;
constexpr size_t MIC_CHUNK_SAMPLES = 960;
constexpr size_t SPEAKER_CHUNK_SAMPLES = 2400;
constexpr float STEP_THRESHOLD_G = 0.22f;
constexpr uint32_t STEP_REFRACTORY_MS = 280;
constexpr uint32_t POCKET_TOUCH_TIMEOUT_MS = 5000;

enum class Mood : uint8_t { Happy, Curious, Sleepy, Surprised, Grumpy };
enum class Screen : uint8_t { Face, Home, Status, Settings };

struct TouchGesture {
  bool tracking = false;
  int startX = 0;
  int startY = 0;
  int x = 0;
  int y = 0;
  uint32_t startedAt = 0;
  bool voiceStarted = false;
};

M5Canvas canvas(&M5.Display);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebSocketsClient hubSocket;
WiFiManager wifiManager;
Preferences preferences;
TouchGesture touch;
Mood mood = Mood::Happy;
Screen screen = Screen::Face;
uint32_t nextBlinkAt = 0;
uint32_t blinkStartedAt = 0;
uint32_t startledUntil = 0;
uint32_t lastStatusPublishAt = 0;
uint32_t lastWiFiAttemptAt = 0;
uint32_t lastMqttAttemptAt = 0;
bool blinking = false;
bool discoveryPublished = false;
bool hubConnected = false;
bool hubUsingCloudflare = false;
bool voiceRecording = false;
bool voiceSpeaking = false;
bool conversationActive = false;
bool resumeListeningPending = false;
uint32_t resumeListeningAt = 0;
bool speakerReady = false;
uint8_t micRecordIndex = 2;
uint8_t micSendIndex = 0;
uint8_t micWarmup = 0;
uint16_t voiceChunksSent = 0;
uint8_t speakerBufferIndex = 0;
int16_t micBuffers[3][MIC_CHUNK_SAMPLES] = {};
int16_t speakerBuffers[3][SPEAKER_CHUNK_SAMPLES] = {};
uint32_t lastHubStatusAt = 0;
String hubAuthHeader;
float gazeX = 0.0f;
float gazeY = 0.0f;
float targetGazeX = 0.0f;
float targetGazeY = 0.0f;
float touchGlow = 0.0f;
String notification;
uint32_t notificationUntil = 0;
uint8_t speakerVolume = AppConfig::SPEAKER_VOLUME;
uint8_t screenBrightness = 120;
uint8_t voiceIndex = 0;
uint32_t wifiConfirmUntil = 0;
String wifiPortalName;
uint32_t stepsTotal = 0;
uint32_t sentStepsTotal = 0;
bool stepAboveThreshold = false;
uint32_t lastStepAt = 0;
const char* orientation = "unknown";
bool pocketed = false;
uint32_t lastTouchAt = 0;
bool listeningEnabled = true;
bool cameraEnabled = true;

constexpr const char* VOICE_IDS[] = {"cedar", "ash", "echo", "verse", "edge"};
constexpr const char* VOICE_LABELS[] = {"Cedar", "Ash", "Echo", "Verse", "Edge TTS"};
constexpr uint8_t VOICE_COUNT = sizeof(VOICE_IDS) / sizeof(VOICE_IDS[0]);

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
    case Screen::Settings: return "settings";
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
  else if (value == "settings") result = Screen::Settings;
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
  if (value < 0) value = 3;
  if (value > 3) value = 0;
  setScreen(static_cast<Screen>(value));
}

void showNotification(const String& message) {
  notification = message.substring(0, 64);
  notificationUntil = millis() + 8000;
}

void saveSettings() {
  preferences.putUChar("volume", speakerVolume);
  preferences.putUChar("brightness", screenBrightness);
  preferences.putUChar("voice", voiceIndex);
}

void saveCapabilities() {
  preferences.putBool("cap_listen", listeningEnabled);
  preferences.putBool("cap_camera", cameraEnabled);
}

void sendCapabilities() {
  if (!hubConnected) return;
  hubSocket.sendTXT(String("{\"type\":\"capabilities\",\"listening\":") +
                    (listeningEnabled ? "true" : "false") +
                    ",\"camera\":" + (cameraEnabled ? "true" : "false") + "}");
}

void sendDeviceSettings() {
  if (!hubConnected) return;
  hubSocket.sendTXT(String("{\"type\":\"settings\",\"voice\":\"") +
                    VOICE_IDS[voiceIndex] + "\"}");
}

void beginListening() {
  if (!listeningEnabled || !conversationActive || voiceRecording || !hubConnected ||
      M5.Speaker.isPlaying()) return;
  M5.Speaker.end();
  speakerReady = false;
  M5.Mic.begin();
  memset(micBuffers, 0, sizeof(micBuffers));
  voiceChunksSent = 0;
  voiceRecording = true;
  voiceSpeaking = false;
  setMood(Mood::Curious, false);
}

void pauseListening() {
  if (!voiceRecording) return;
  voiceRecording = false;
  while (M5.Mic.isRecording()) delay(1);
  M5.Mic.end();
}

void stopConversation(const char* reason) {
  if (!conversationActive) return;
  conversationActive = false;
  resumeListeningPending = false;
  pauseListening();
  M5.Speaker.end();
  speakerReady = false;
  voiceSpeaking = false;
  if (hubConnected) hubSocket.sendTXT("{\"type\":\"conversation_stop\"}");
  setMood(Mood::Happy, false);
  showNotification(reason);
}

void toggleConversation() {
  if (conversationActive) {
    stopConversation("Conversation ended");
    return;
  }
  if (!listeningEnabled) {
    showNotification("Listening is off");
    return;
  }
  if (!hubConnected) {
    showNotification("Taco Hub is offline");
    return;
  }
  conversationActive = true;
  startledUntil = millis() + 650;
  hubSocket.sendTXT("{\"type\":\"conversation_start\"}");
  beginListening();
  showNotification("Listening... tap to stop");
}

void onHubEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      hubConnected = true;
      hubSocket.sendTXT(String("{\"type\":\"hello\",\"device_id\":\"") +
                        AppConfig::DEVICE_ID +
                        "\",\"hardware\":\"CoreS3\",\"firmware\":\"1.0.0-alpha.6\"}");
      sendDeviceSettings();
      sendCapabilities();
      showNotification("Taco Hub connected");
      break;
    case WStype_DISCONNECTED:
      hubConnected = false;
      conversationActive = false;
      resumeListeningPending = false;
      pauseListening();
      break;
    case WStype_TEXT:
      if (!length) break;
      {
        String message;
        message.reserve(length + 1);
        for (size_t i = 0; i < length; ++i) message += static_cast<char>(payload[i]);
        if (message.indexOf("hello_ack") >= 0) {
          setMood(Mood::Happy, false);
        } else if (message.indexOf("\"state\":\"listening\"") >= 0) {
          if (conversationActive) {
            resumeListeningPending = true;
            resumeListeningAt = millis() + 120;
            beginListening();
          }
          showNotification(conversationActive ? "Listening... tap to stop" : "Listening...");
        } else if (message.indexOf("\"state\":\"thinking\"") >= 0) {
          pauseListening();
          setMood(Mood::Sleepy, false);
          showNotification("Thinking...");
        } else if (message.indexOf("\"state\":\"speaking\"") >= 0) {
          pauseListening();
          voiceSpeaking = true;
          setMood(Mood::Happy, false);
          showNotification("Taco is speaking");
        } else if (message.indexOf("\"state\":\"idle\"") >= 0) {
          pauseListening();
          resumeListeningPending = false;
          voiceSpeaking = false;
          setMood(Mood::Happy, false);
          notificationUntil = 0;
        } else if (message.indexOf("\"state\":\"error\"") >= 0) {
          voiceSpeaking = false;
          setMood(Mood::Grumpy, false);
          showNotification("Voice service error");
        } else if (message.indexOf("\"type\":\"capabilities_set\"") >= 0) {
          const bool wasListening = listeningEnabled;
          if (message.indexOf("\"listening\":true") >= 0) listeningEnabled = true;
          else if (message.indexOf("\"listening\":false") >= 0) listeningEnabled = false;
          if (message.indexOf("\"camera\":true") >= 0) cameraEnabled = true;
          else if (message.indexOf("\"camera\":false") >= 0) cameraEnabled = false;
          if (wasListening != listeningEnabled) {
            saveCapabilities();
            if (!listeningEnabled) stopConversation("Listening disabled");
            else showNotification("Listening enabled");
          }
        }
      }
      break;
    case WStype_BIN: {
      if (!length || voiceRecording) break;
      M5.Mic.end();
      if (!speakerReady) {
        M5.Speaker.begin();
        M5.Speaker.setVolume(speakerVolume);
        M5.Speaker.setAllChannelVolume(255);
        speakerReady = true;
      }
      const size_t samples = min(length / sizeof(int16_t), SPEAKER_CHUNK_SAMPLES);
      auto* output = speakerBuffers[speakerBufferIndex];
      memcpy(output, payload, samples * sizeof(int16_t));
      M5.Speaker.playRaw(output, samples, VOICE_SAMPLE_RATE, false, 1, 0);
      speakerBufferIndex = (speakerBufferIndex + 1) % 3;
      break;
    }
    default:
      break;
  }
}

void serviceMotion(uint32_t now) {
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
  const float magnitude = sqrtf(ax * ax + ay * ay + az * az);

  if (!stepAboveThreshold && magnitude > 1.0f + STEP_THRESHOLD_G) {
    stepAboveThreshold = true;
  } else if (stepAboveThreshold && magnitude < 1.0f - STEP_THRESHOLD_G * 0.5f) {
    stepAboveThreshold = false;
    if (now - lastStepAt > STEP_REFRACTORY_MS) {
      ++stepsTotal;
      lastStepAt = now;
    }
  }

  const bool flat = fabsf(ax) < 0.35f && fabsf(ay) < 0.35f && fabsf(az) > 0.75f;
  orientation = flat ? (az > 0 ? "face_up" : "face_down") : "on_side";

  pocketed = strcmp(orientation, "face_up") != 0 && !voiceRecording && !voiceSpeaking &&
             (now - lastTouchAt > POCKET_TOUCH_TIMEOUT_MS);
}

void serviceVoice() {
  if (!voiceRecording || !hubConnected) return;
  auto* input = micBuffers[0];
  if (M5.Mic.record(input, MIC_CHUNK_SAMPLES, VOICE_SAMPLE_RATE, false)) {
    while (voiceRecording && M5.Mic.isRecording()) delay(1);
    if (voiceRecording) {
      hubSocket.sendBIN(reinterpret_cast<uint8_t*>(input),
                        MIC_CHUNK_SAMPLES * sizeof(int16_t));
      ++voiceChunksSent;
    }
  }
}

void serviceHub(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) return;
  hubSocket.loop();
  if (resumeListeningPending && conversationActive && now >= resumeListeningAt &&
      !M5.Speaker.isPlaying()) {
    resumeListeningPending = false;
    beginListening();
  }
  if (!hubConnected || now - lastHubStatusAt < 15000) return;
  lastHubStatusAt = now;
  const uint32_t stepsDelta = stepsTotal - sentStepsTotal;
  sentStepsTotal = stepsTotal;
  String status = String("{\"type\":\"status\",\"battery\":") +
                  M5.Power.getBatteryLevel() + ",\"rssi\":" + WiFi.RSSI() +
                  ",\"uptime\":" + now / 1000 +
                  ",\"screen\":\"" + screenName(screen) + "\"" +
                  ",\"steps_total\":" + stepsTotal +
                  ",\"steps_delta\":" + stepsDelta +
                  ",\"orientation\":\"" + orientation + "\"" +
                  ",\"pocketed\":" + (pocketed ? "true" : "false") + "}";
  hubSocket.sendTXT(status);
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

void drawCheekMarks(int centerX, int centerY, int side) {
  const int baseX = centerX + side * 48;
  canvas.drawWideLine(baseX, centerY + 28, baseX + side * 9, centerY + 22, 4, PINK);
  canvas.drawWideLine(baseX + side * 9, centerY + 31,
                      baseX + side * 18, centerY + 25, 4, PINK);
}

void drawEye(int centerX, int centerY, float open, float lookX, float lookY, int side) {
  if (mood == Mood::Sleepy && !blinking) {
    canvas.drawArc(centerX, centerY + 5, 35, 21, 18, 162, CYAN_DIM);
    canvas.drawArc(centerX, centerY + 5, 32, 18, 18, 162, CYAN);
    drawCheekMarks(centerX, centerY, side);
    return;
  }
  int baseRadius = mood == Mood::Surprised ? 39 : 36;
  if (mood == Mood::Curious) baseRadius += side < 0 ? 5 : -2;
  const int radius = max(3, static_cast<int>(baseRadius * open));
  canvas.fillCircle(centerX, centerY, radius + 8, PANEL_ACTIVE);
  canvas.fillCircle(centerX, centerY, radius + 4, CYAN_DIM);
  canvas.fillCircle(centerX, centerY, radius, CYAN);
  if (radius > 9) {
    const int pupilX = centerX + static_cast<int>(lookX * 8.0f);
    const int pupilY = centerY + static_cast<int>(lookY * 7.0f);
    const int shadowX = pupilX + radius / 3;
    const int shadowY = pupilY + radius / 3;
    canvas.fillCircle(shadowX, shadowY, static_cast<int>(radius * 0.78f), BG);
    canvas.fillCircle(centerX - radius / 3, centerY - radius / 3, max(3, radius / 6), WHITE);
    canvas.fillCircle(centerX - 2, centerY - 3, max(2, radius / 10), BLUE_WHITE);
  }
  drawCheekMarks(centerX, centerY, side);
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
  if (voiceSpeaking) {
    const int height = 8 + static_cast<int>((sinf(millis() * 0.018f) + 1.0f) * 7.0f);
    canvas.fillRoundRect(142, 177 - height / 2, 36, height, height / 2, CYAN);
    return;
  }
  switch (mood) {
    case Mood::Happy: canvas.drawArc(160, 168, 28, 23, 20, 160, CYAN); break;
    case Mood::Curious: canvas.fillCircle(160, 179, 7, CYAN); break;
    case Mood::Sleepy: canvas.drawWideLine(143, 180, 177, 180, 5, CYAN); break;
    case Mood::Surprised: canvas.drawCircle(160, 179, 13, CYAN); break;
    case Mood::Grumpy: canvas.drawArc(160, 196, 25, 18, 200, 340, CYAN); break;
  }
}

void drawPageDots() {
  for (int i = 0; i < 4; ++i) {
    const int x = 142 + i * 12;
    const bool active = i == static_cast<int>(screen);
    canvas.fillCircle(x, 229, active ? 3 : 2, active ? CYAN : CYAN_DIM);
  }
}

void drawSettingRow(int y, const char* label, const String& value,
                    bool adjustable = true) {
  canvas.fillRoundRect(14, y, 292, 39, 11, PANEL);
  canvas.setTextDatum(middle_left);
  canvas.setTextSize(1);
  canvas.setTextColor(WHITE, PANEL);
  canvas.drawString(label, 27, y + 20);
  canvas.setTextDatum(middle_right);
  canvas.setTextColor(CYAN, PANEL);
  canvas.drawString(value, 279, y + 20);
  if (adjustable) {
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(MUTED, PANEL);
    canvas.drawString("<", 180, y + 20);
    canvas.drawString(">", 294, y + 20);
  }
}

void drawNotification();
void drawHeader(const char* title);

void drawSettings() {
  canvas.fillScreen(BG);
  drawHeader("SETTINGS");
  drawSettingRow(44, "Volume", String((speakerVolume * 100) / 255) + "%");
  drawSettingRow(87, "Brightness", String((screenBrightness * 100) / 255) + "%");
  drawSettingRow(130, "Voice", VOICE_LABELS[voiceIndex]);
  drawSettingRow(173, "Wi-Fi", "Change network", false);
  drawNotification();
  drawPageDots();
}

void drawNotification() {
  if (notification.isEmpty() || millis() >= notificationUntil) return;
  canvas.fillRoundRect(18, 182, 284, 36, 12, PANEL_ACTIVE);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(WHITE, PANEL_ACTIVE);
  canvas.setTextSize(1);
  canvas.drawString(notification, 160, 200);
}

void drawMoodAccent() {
  if (mood == Mood::Sleepy) {
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(CYAN, BG);
    canvas.setTextSize(1);
    canvas.drawString("z", 238, 82);
    canvas.setTextSize(2);
    canvas.drawString("Z", 252, 66);
  } else if (mood == Mood::Curious) {
    canvas.drawWideLine(232, 62, 232, 76, 2, CYAN);
    canvas.drawWideLine(225, 69, 239, 69, 2, CYAN);
  }
}

void drawFace(uint32_t now) {
  if (blinking && now - blinkStartedAt >= 220) blinking = false;
  const uint8_t* start = faceHappyStart;
  const uint8_t* end = faceHappyEnd;

  if (now < startledUntil) {
    start = faceStartledStart;
    end = faceStartledEnd;
  } else if (voiceSpeaking) {
    start = faceExcitedStart;
    end = faceExcitedEnd;
  } else if (blinking && !conversationActive && mood == Mood::Happy) {
    start = faceWinkStart;
    end = faceWinkEnd;
  } else {
    switch (mood) {
      case Mood::Curious:
        start = faceCuriousStart;
        end = faceCuriousEnd;
        break;
      case Mood::Sleepy:
        start = faceSleepyStart;
        end = faceSleepyEnd;
        break;
      case Mood::Surprised:
        start = faceSurprisedStart;
        end = faceSurprisedEnd;
        break;
      case Mood::Grumpy:
        start = faceSadStart;
        end = faceSadEnd;
        break;
      case Mood::Happy:
        break;
    }
  }

  canvas.drawJpg(start, static_cast<size_t>(end - start), 0, 0, 320, 240);
  canvas.fillCircle(309, 11, 4,
                    hubConnected ? (conversationActive ? CYAN : CYAN_DIM) : PINK);
  if (!notification.isEmpty() && now < notificationUntil) {
    canvas.fillRoundRect(38, 218, 244, 18, 9, PANEL);
    canvas.setTextDatum(middle_center);
    canvas.setTextSize(1);
    canvas.setTextColor(WHITE, PANEL);
    canvas.drawString(notification.substring(0, 38), 160, 227);
  }
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
  drawStatusRow(141, "Taco Hub",
                hubConnected ? (hubUsingCloudflare ? "Cloud" : "Local")
                             : "Not connected");
  drawStatusRow(178, "IP address",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "--");
  drawPageDots();
}

void drawCurrentScreen(uint32_t now) {
  switch (screen) {
    case Screen::Face: drawFace(now); break;
    case Screen::Home: drawHome(); break;
    case Screen::Status: drawStatus(); break;
    case Screen::Settings: drawSettings(); break;
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
  if (screen == Screen::Face) toggleConversation();
  else if (screen == Screen::Home) triggerHomeAction(x, y);
  else if (screen == Screen::Settings) {
    const int direction = x < 235 ? -1 : 1;
    if (y >= 44 && y < 84) {
      int value = speakerVolume + direction * 26;
      speakerVolume = static_cast<uint8_t>(constrain(value, 25, 255));
      M5.Speaker.setVolume(speakerVolume);
      saveSettings();
    } else if (y >= 87 && y < 127) {
      int value = screenBrightness + direction * 25;
      screenBrightness = static_cast<uint8_t>(constrain(value, 25, 255));
      M5.Display.setBrightness(screenBrightness);
      saveSettings();
    } else if (y >= 130 && y < 170) {
      int value = static_cast<int>(voiceIndex) + direction;
      if (value < 0) value = VOICE_COUNT - 1;
      if (value >= VOICE_COUNT) value = 0;
      voiceIndex = static_cast<uint8_t>(value);
      saveSettings();
      sendDeviceSettings();
      showNotification(String("Voice: ") + VOICE_LABELS[voiceIndex]);
    } else if (y >= 173 && y < 216) {
      if (millis() > wifiConfirmUntil) {
        wifiConfirmUntil = millis() + 4000;
        showNotification("Tap Wi-Fi again to change");
      } else {
        canvas.fillScreen(BG);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(CYAN, BG);
        canvas.setTextSize(2);
        canvas.drawString("WI-FI SETUP", 160, 70);
        canvas.setTextSize(1);
        canvas.setTextColor(WHITE, BG);
        canvas.drawString(String("Connect to ") + wifiPortalName, 160, 120);
        canvas.drawString("Open 192.168.4.1", 160, 150);
        canvas.pushSprite(0, 0);
        hubSocket.disconnect();
        wifiManager.setConfigPortalTimeout(180);
        wifiManager.startConfigPortal(wifiPortalName.c_str());
        ESP.restart();
      }
    }
  }
}

void handleTouch() {
  const auto count = M5.Touch.getCount();
  if (!count) {
    targetGazeX *= 0.92f;
    targetGazeY *= 0.92f;
    return;
  }
  lastTouchAt = millis();
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
    touch.voiceStarted = false;
  }
  if (touch.tracking && detail.isPressed()) {
    touch.x = detail.x;
    touch.y = detail.y;
    const int moveX = detail.x - touch.startX;
    const int moveY = detail.y - touch.startY;
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
  preferences.begin("taco", false);
  speakerVolume = preferences.getUChar("volume", AppConfig::SPEAKER_VOLUME);
  screenBrightness = preferences.getUChar("brightness", 120);
  voiceIndex = preferences.getUChar("voice", 0);
  if (voiceIndex >= VOICE_COUNT) voiceIndex = 0;
  listeningEnabled = preferences.getBool("cap_listen", true);
  cameraEnabled = preferences.getBool("cap_camera", true);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(screenBrightness);
  M5.Speaker.setVolume(speakerVolume);
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
  WiFi.mode(WIFI_STA);
  wifiManager.setConfigPortalTimeout(180);
  wifiPortalName = String("Taco-") +
                   String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX).substring(4);
  canvas.fillScreen(BG);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_CYAN, TFT_BLACK);
  canvas.setTextSize(2);
  canvas.drawString("WI-FI SETUP", 160, 62);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.setTextSize(1);
  canvas.drawString(String("Connect to ") + wifiPortalName, 160, 112);
  canvas.setTextColor(0x7BEF, TFT_BLACK);
  canvas.drawString("Then open 192.168.4.1", 160, 142);
  canvas.drawString("Taco will remember your network", 160, 174);
  canvas.pushSprite(0, 0);
  if (!wifiManager.autoConnect(wifiPortalName.c_str())) {
    showNotification("Wi-Fi setup needed");
  }
  if (WiFi.status() == WL_CONNECTED && AppConfig::DEVICE_TOKEN[0] != '\0') {
    hubAuthHeader = String("Authorization: Bearer ") + AppConfig::DEVICE_TOKEN + "\r\n";
    hubSocket.setExtraHeaders(hubAuthHeader.c_str());
    hubSocket.onEvent(onHubEvent);
    hubSocket.setReconnectInterval(3000);
    WiFiClient hubProbe;
    const bool localHubAvailable = AppConfig::HUB_HOST[0] != '\0' &&
                                   hubProbe.connect(AppConfig::HUB_HOST,
                                                    AppConfig::HUB_PORT, 750);
    hubProbe.stop();
    if (localHubAvailable) {
      hubSocket.begin(AppConfig::HUB_HOST, AppConfig::HUB_PORT,
                      AppConfig::HUB_PATH);
    } else {
      hubUsingCloudflare = true;
      hubSocket.beginSSL(AppConfig::HUB_REMOTE_HOST, AppConfig::HUB_REMOTE_PORT,
                         AppConfig::HUB_PATH);
    }
  }
  nextBlinkAt = millis() + 1800;
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  handleTouch();
  serviceMotion(now);
  serviceHub(now);
  serviceVoice();
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
