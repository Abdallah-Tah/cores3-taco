# Taco device protocol v1

CoreS3 devices connect to `/device/v1` over WebSocket and authenticate with an
`Authorization: Bearer <device-token>` header. JSON control messages use a
required `type` field. Binary audio frames will be introduced without changing
the control envelope.

## Connection

The device first sends `hello`:

```json
{"type":"hello","device_id":"cores3-4d5184","hardware":"CoreS3","firmware":"1.0.0-alpha.2"}
```

The hub returns `hello_ack`. The device then sends `status` every 15 seconds
with battery percentage, Wi-Fi RSSI, uptime, and active screen. Either side may
send `ping`; the receiver answers with `pong`.

API credentials for OpenAI, OpenClaw, and Home Assistant are never sent to or
stored on the CoreS3.
