#pragma once

#if __has_include("Secrets.h")
#include "Secrets.h"
#endif

#ifndef COMPANION_WIFI_SSID
#define COMPANION_WIFI_SSID ""
#endif
#ifndef COMPANION_WIFI_PASSWORD
#define COMPANION_WIFI_PASSWORD ""
#endif
#ifndef COMPANION_MQTT_HOST
#define COMPANION_MQTT_HOST ""
#endif
#ifndef COMPANION_MQTT_PORT
#define COMPANION_MQTT_PORT 1883
#endif
#ifndef COMPANION_MQTT_USERNAME
#define COMPANION_MQTT_USERNAME ""
#endif
#ifndef COMPANION_MQTT_PASSWORD
#define COMPANION_MQTT_PASSWORD ""
#endif

namespace AppConfig {

constexpr char WIFI_SSID[] = COMPANION_WIFI_SSID;
constexpr char WIFI_PASSWORD[] = COMPANION_WIFI_PASSWORD;

constexpr char MQTT_HOST[] = COMPANION_MQTT_HOST;
constexpr uint16_t MQTT_PORT = COMPANION_MQTT_PORT;
constexpr char MQTT_USERNAME[] = COMPANION_MQTT_USERNAME;
constexpr char MQTT_PASSWORD[] = COMPANION_MQTT_PASSWORD;

constexpr char DEVICE_NAME[] = "CoreS3 Companion";
constexpr char DEVICE_ID[] = "cores3_companion";
constexpr char TOPIC_PREFIX[] = "cores3/companion";

// Home Assistant receives an event on cores3/companion/action when tapped.
constexpr char ACTION_1[] = "Lights";
constexpr char ACTION_2[] = "Scene";
constexpr char ACTION_3[] = "Climate";
constexpr char ACTION_4[] = "Assist";

}  // namespace AppConfig
