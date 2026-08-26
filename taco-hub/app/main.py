from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import uuid
from datetime import UTC, datetime
from typing import Any

from fastapi import Body, FastAPI, WebSocket, WebSocketDisconnect, status

from . import storage
from .capabilities import DEFAULT_CAPABILITIES, HARDWARE_KEYS, validate_updates
from .realtime import RealtimeBridge

logging.basicConfig(level=os.getenv("TACO_LOG_LEVEL", "INFO"))
logger = logging.getLogger("taco-hub")

app = FastAPI(title="Taco Hub", version="0.1.0")
devices: dict[str, dict[str, Any]] = {}

STEP_MILESTONE = 1000


@app.on_event("startup")
async def on_startup() -> None:
    storage.init_db(DEFAULT_CAPABILITIES)


def utc_now() -> str:
    return datetime.now(UTC).isoformat()


def authorized(websocket: WebSocket) -> bool:
    expected = os.getenv("TACO_DEVICE_TOKEN", "")
    supplied = websocket.headers.get("authorization", "")
    if not expected or not supplied.startswith("Bearer "):
        return False
    return hmac.compare_digest(supplied[7:], expected)


@app.get("/health")
async def health() -> dict[str, Any]:
    return {
        "status": "ok",
        "service": "taco-hub",
        "time": utc_now(),
        "connected_devices": len(devices),
    }


@app.get("/api/v1/devices")
async def list_devices() -> dict[str, Any]:
    return {
        "devices": [
            {k: v for k, v in device.items() if k != "send_json"}
            for device in devices.values()
        ]
    }


@app.get("/api/v1/capabilities")
async def get_capabilities() -> dict[str, Any]:
    return {"capabilities": storage.get_capabilities()}


@app.post("/api/v1/capabilities")
async def set_capabilities(updates: dict[str, Any] = Body(...)) -> dict[str, Any]:
    cleaned = validate_updates(updates)
    result = storage.set_capabilities(cleaned)
    hardware_updates = {k: v for k, v in cleaned.items() if k in HARDWARE_KEYS}
    if hardware_updates:
        for device in devices.values():
            send = device.get("send_json")
            if send:
                await send({"type": "capabilities_set", **hardware_updates})
    return {"capabilities": result}


@app.get("/api/v1/journal")
async def get_journal(limit: int = 100, device_id: str | None = None) -> dict[str, Any]:
    return {"journal": storage.list_journal(limit=limit, device_id=device_id)}


@app.websocket("/device/v1")
async def device_socket(websocket: WebSocket) -> None:
    if not authorized(websocket):
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        return

    await websocket.accept()
    device_id = "unknown"
    session_id = uuid.uuid4().hex
    send_lock = asyncio.Lock()

    async def send_json(payload: dict) -> None:
        async with send_lock:
            await websocket.send_json(payload)

    async def send_bytes(payload: bytes) -> None:
        async with send_lock:
            await websocket.send_bytes(payload)

    realtime = RealtimeBridge(send_json, send_bytes)
    try:
        hello = await asyncio.wait_for(websocket.receive_json(), timeout=10)
        if hello.get("type") != "hello" or not hello.get("device_id"):
            await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
            return

        device_id = str(hello["device_id"])
        devices[device_id] = {
            "session_id": session_id,
            "device_id": device_id,
            "firmware": hello.get("firmware", "unknown"),
            "hardware": hello.get("hardware", "unknown"),
            "ip": websocket.client.host if websocket.client else "unknown",
            "connected_at": utc_now(),
            "last_seen": utc_now(),
            "status": {},
            "send_json": send_json,
        }
        logger.info("device connected: %s", device_id)
        storage.add_journal_entry(device_id, "device_connected", {})
        await send_json(
            {"type": "hello_ack", "server": "taco-hub", "time": utc_now()}
        )
        hardware_state = {
            key: value
            for key, value in storage.get_capabilities().items()
            if key in HARDWARE_KEYS
        }
        if hardware_state:
            await send_json({"type": "capabilities_set", **hardware_state})

        while True:
            message = await websocket.receive()
            if message["type"] == "websocket.disconnect":
                raise WebSocketDisconnect(message.get("code", 1000))
            device = devices.get(device_id)
            if not device or device.get("session_id") != session_id:
                await websocket.close(code=status.WS_1000_NORMAL_CLOSURE)
                return
            device["last_seen"] = utc_now()
            if message.get("bytes") is not None:
                await realtime.append_audio(message["bytes"])
                continue
            payload = json.loads(message.get("text") or "{}")
            if payload.get("type") == "status":
                device["status"] = payload
                if any(
                    key in payload
                    for key in ("steps_total", "orientation", "pocketed")
                ):
                    await handle_sense(device_id, payload)
                await send_json({"type": "status_ack", "time": utc_now()})
            elif payload.get("type") == "ping":
                await send_json({"type": "pong", "time": utc_now()})
            elif payload.get("type") == "voice_start":
                await realtime.start_turn()
            elif payload.get("type") == "voice_end":
                await realtime.finish_turn()
            elif payload.get("type") == "voice_cancel":
                await realtime.cancel_turn()
            elif payload.get("type") == "conversation_start":
                await realtime.start_conversation()
            elif payload.get("type") == "conversation_stop":
                await realtime.stop_conversation()
            elif payload.get("type") == "settings":
                await realtime.set_voice(str(payload.get("voice", "cedar")))
                device["voice"] = realtime.voice
            elif payload.get("type") == "capabilities":
                updates = validate_updates(
                    {k: v for k, v in payload.items() if k in HARDWARE_KEYS}
                )
                if updates:
                    storage.set_capabilities(updates)
                    storage.add_journal_entry(
                        device_id, "capability_changed", updates
                    )
    except (WebSocketDisconnect, TimeoutError, json.JSONDecodeError):
        pass
    finally:
        await realtime.close()
        if devices.get(device_id, {}).get("session_id") == session_id:
            devices.pop(device_id, None)
        if device_id != "unknown":
            storage.add_journal_entry(device_id, "device_disconnected", {})
        logger.info("device disconnected: %s", device_id)


async def handle_sense(device_id: str, payload: dict[str, Any]) -> None:
    steps_total = payload.get("steps_total")
    steps_delta = payload.get("steps_delta")
    orientation = payload.get("orientation")
    pocketed = payload.get("pocketed")
    storage.record_sensor_event(
        device_id, steps_total, steps_delta, orientation, pocketed, payload
    )

    device = devices.get(device_id)
    if device is None:
        return
    previous = device.get("sense", {})

    if pocketed is not None and previous.get("pocketed") != pocketed:
        storage.add_journal_entry(
            device_id, "pocketed" if pocketed else "unpocketed", {}
        )

    if isinstance(steps_total, int):
        prev_total = previous.get("steps_total")
        if isinstance(prev_total, int) and (
            steps_total // STEP_MILESTONE > prev_total // STEP_MILESTONE
        ):
            milestone = (steps_total // STEP_MILESTONE) * STEP_MILESTONE
            storage.add_journal_entry(
                device_id, "steps_milestone", {"steps_total": milestone}
            )

    device["sense"] = {
        "steps_total": steps_total,
        "orientation": orientation,
        "pocketed": pocketed,
    }
