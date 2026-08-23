from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import uuid
from datetime import UTC, datetime
from typing import Any

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, status

from .realtime import RealtimeBridge

logging.basicConfig(level=os.getenv("TACO_LOG_LEVEL", "INFO"))
logger = logging.getLogger("taco-hub")

app = FastAPI(title="Taco Hub", version="0.1.0")
devices: dict[str, dict[str, Any]] = {}


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
    return {"devices": list(devices.values())}


@app.websocket("/device/v1")
async def device_socket(websocket: WebSocket) -> None:
    if not authorized(websocket):
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        return

    await websocket.accept()
    device_id = "unknown"
    session_id = uuid.uuid4().hex
    realtime = RealtimeBridge(websocket.send_json, websocket.send_bytes)
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
        }
        logger.info("device connected: %s", device_id)
        await websocket.send_json(
            {"type": "hello_ack", "server": "taco-hub", "time": utc_now()}
        )

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
                await websocket.send_json({"type": "status_ack", "time": utc_now()})
            elif payload.get("type") == "ping":
                await websocket.send_json({"type": "pong", "time": utc_now()})
            elif payload.get("type") == "voice_start":
                await realtime.start_turn()
            elif payload.get("type") == "voice_end":
                await realtime.finish_turn()
    except (WebSocketDisconnect, TimeoutError, json.JSONDecodeError):
        pass
    finally:
        await realtime.close()
        if devices.get(device_id, {}).get("session_id") == session_id:
            devices.pop(device_id, None)
        logger.info("device disconnected: %s", device_id)
