# CoreS3 Taco

Taco is a purpose-built, expressive AI companion for the M5Stack CoreS3.
The project combines a native ESP-IDF device experience with a secure
Raspberry Pi gateway for OpenAI Realtime voice, OpenClaw, Home Assistant,
vision, memory, weather, reminders, and system monitoring.

## Project direction

Taco is being separated from the XiaoZhi runtime into an independent firmware
and service architecture. API keys and home credentials remain on the
Raspberry Pi; the CoreS3 owns the interface, audio, camera, sensors, and local
interactions.

```text
CoreS3 Taco
  ├── face, touch, audio, camera, sensors
  └── secure WebSocket
          └── Raspberry Pi Taco Hub
                ├── OpenAI Realtime
                ├── OpenClaw
                ├── Home Assistant
                └── device services and monitoring
```

## Current milestone

- Animated face with blinking and emotional expressions
- Touch gestures and Home/Status screens
- Captive-portal Wi-Fi provisioning
- Authenticated CoreS3-to-Pi WebSocket connection
- Battery, Wi-Fi, uptime, and screen-state heartbeat
- Systemd-managed Taco Hub with health and device APIs

The current PlatformIO firmware lives at the repository root. The Pi gateway
is in `taco-hub/`, and the device protocol is documented in
`protocol/device-v1.md`. The older XiaoZhi-derived source under `firmware/` is
reference material and is not the active build.

## Roadmap

1. Clean Taco firmware foundation and secure Pi protocol
2. Low-latency voice with OpenAI Realtime and Edge TTS fallback
3. OpenClaw memory and Home Assistant tools
4. Wake word, camera vision, proximity, light, and motion reactions
5. Clock, weather, alarms, reminders, Pi monitoring, and OTA updates

## Build firmware

Copy `include/Secrets.example.h` to the ignored `include/Secrets.h`, generate a
long random device token, then build with PlatformIO:

```sh
pio run
pio run --target upload --upload-port /dev/cu.usbmodem101
```

The same `TACO_DEVICE_TOKEN` must be stored in the Pi service environment.
Generated builds, managed components, local SDKs, credentials, and device
backups are intentionally excluded from version control.

Hardware target: M5Stack CoreS3 / ESP32-S3, 16 MB flash, 8 MB PSRAM.

## Security

Never commit Wi-Fi passwords, OpenAI API keys, Home Assistant tokens, device
certificates, or public-endpoint secrets. Device credentials must be provisioned
outside the source tree.
