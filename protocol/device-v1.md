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

## Voice

On the face screen, holding for 400 ms starts a push-to-talk turn. The device
sends `voice_start`, followed by binary 24 kHz mono PCM16 chunks, followed by
`voice_end` on release. Taco Hub returns `voice_state` JSON messages and binary
24 kHz mono PCM16 response chunks. Output chunks are limited to 4,800 bytes so
the CoreS3 can maintain a small, deterministic speaker buffer ring.

The device sends `settings` with its selected voice after connecting and when
the user changes it. Taco Hub validates the voice and applies it to the next
Realtime session.

At startup, Taco probes the private LAN hub briefly. If it is unavailable, the
device connects over TLS to `taco-hub.buildwithabdallah.com`. The same device
token authenticates both routes; the Cloudflare Tunnel does not require an
inbound Pi firewall port.
