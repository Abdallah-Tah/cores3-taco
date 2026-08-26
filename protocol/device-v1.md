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

## Capabilities

Privacy-sensitive capabilities (`listening`, `camera`) are enforced on the
device itself; the hub's copy reflects the last known device state and is
only used to push changes back to the device. Software-only capabilities
(`memory`, `habit_tracking`, `cloud_ai`, `kids_mode`, `proactive`) are owned
by the hub and are not part of the device protocol.

The device sends `capabilities` on connect and whenever a toggle changes:

```json
{"type":"capabilities","listening":true,"camera":true}
```

The hub may push `capabilities_set` at any time (e.g. after a change via its
`/api/v1/capabilities` HTTP API) to change a device-side toggle remotely:

```json
{"type":"capabilities_set","listening":false}
```

When `listening` is set to `false`, the device stops any active
conversation and will not start a new voice turn until it is re-enabled,
either from the device itself or by another `capabilities_set` message.

## Senses

The device augments its periodic `status` message with a sensor summary
computed on-device from its built-in IMU. Raw motion samples are never
streamed to the hub — only the derived summary:

```json
{
  "type": "status",
  "battery": 82,
  "rssi": -54,
  "uptime": 1234,
  "screen": "face",
  "steps_total": 4213,
  "steps_delta": 37,
  "orientation": "face_up",
  "pocketed": false
}
```

`orientation` is one of `face_up`, `face_down`, `on_side`, or `unknown`.
`pocketed` is a heuristic derived from orientation and touch inactivity —
CoreS3 has no ambient light sensor, so this is an approximate signal, not a
guaranteed detection of the device being in a pocket. The hub records these
into `sensor_events` and appends `pocketed`/`unpocketed` and
`steps_milestone` entries to the event journal on state transitions.
