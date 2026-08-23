from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
from collections.abc import Awaitable, Callable

from websockets.asyncio.client import connect

logger = logging.getLogger("taco-hub.realtime")
SUPPORTED_VOICES = {"cedar", "ash", "echo", "verse"}

SendJson = Callable[[dict], Awaitable[None]]
SendBytes = Callable[[bytes], Awaitable[None]]


class RealtimeBridge:
    """One OpenAI Realtime session bridged to one Taco device."""

    def __init__(self, send_json: SendJson, send_bytes: SendBytes) -> None:
        self.send_json = send_json
        self.send_bytes = send_bytes
        self.socket = None
        self.receiver: asyncio.Task | None = None
        self.speaking = False
        self.voice = os.getenv("TACO_REALTIME_VOICE", "cedar")

    async def set_voice(self, voice: str) -> None:
        if voice not in SUPPORTED_VOICES:
            raise ValueError(f"Unsupported Taco voice: {voice}")
        if voice == self.voice:
            return
        await self.close()
        self.voice = voice

    async def connect(self) -> None:
        if self.socket is not None:
            return
        api_key = os.environ.get("OPENAI_API_KEY", "")
        if not api_key:
            raise RuntimeError("OPENAI_API_KEY is not configured")
        model = os.getenv("TACO_REALTIME_MODEL", "gpt-realtime-2.1")
        url = f"wss://api.openai.com/v1/realtime?model={model}"
        self.socket = await connect(
            url,
            additional_headers={
                "Authorization": f"Bearer {api_key}",
                "OpenAI-Safety-Identifier": "taco-owner",
            },
            max_size=16 * 1024 * 1024,
            ping_interval=20,
        )
        await self.socket.send(
            json.dumps(
                {
                    "type": "session.update",
                    "session": {
                        "type": "realtime",
                        "model": model,
                        "output_modalities": ["audio"],
                        "instructions": (
                            "You are Taco, a warm, clever, concise home companion. "
                            "Speak naturally, answer quickly, and keep most replies under "
                            "three sentences unless the user asks for detail."
                        ),
                        "audio": {
                            "input": {
                                "format": {"type": "audio/pcm", "rate": 24000},
                                "turn_detection": None,
                            },
                            "output": {
                                "format": {"type": "audio/pcm"},
                                "voice": self.voice,
                            },
                        },
                    },
                }
            )
        )
        self.receiver = asyncio.create_task(self._receive())
        logger.info("OpenAI Realtime connected")

    async def start_turn(self) -> None:
        await self.connect()
        await self.socket.send(json.dumps({"type": "input_audio_buffer.clear"}))
        await self.send_json({"type": "voice_state", "state": "listening"})

    async def append_audio(self, pcm16: bytes) -> None:
        if self.socket is None:
            return
        await self.socket.send(
            json.dumps(
                {
                    "type": "input_audio_buffer.append",
                    "audio": base64.b64encode(pcm16).decode("ascii"),
                }
            )
        )

    async def finish_turn(self) -> None:
        if self.socket is None:
            return
        await self.socket.send(json.dumps({"type": "input_audio_buffer.commit"}))
        await self.socket.send(json.dumps({"type": "response.create"}))
        await self.send_json({"type": "voice_state", "state": "thinking"})

    async def _receive(self) -> None:
        try:
            async for raw in self.socket:
                event = json.loads(raw)
                event_type = event.get("type", "")
                if event_type == "response.output_audio.delta":
                    if not self.speaking:
                        self.speaking = True
                        await self.send_json({"type": "voice_state", "state": "speaking"})
                    audio = base64.b64decode(event.get("delta", ""))
                    for offset in range(0, len(audio), 4800):
                        await self.send_bytes(audio[offset : offset + 4800])
                elif event_type == "response.done":
                    self.speaking = False
                    await self.send_json({"type": "voice_state", "state": "idle"})
                elif event_type == "error":
                    message = event.get("error", {}).get("message", "Realtime error")
                    logger.error("OpenAI Realtime error: %s", message)
                    await self.send_json(
                        {"type": "voice_state", "state": "error", "message": message[:80]}
                    )
        except asyncio.CancelledError:
            raise
        except Exception:
            logger.exception("OpenAI Realtime receiver stopped")
            await self.send_json({"type": "voice_state", "state": "error"})
        finally:
            self.socket = None

    async def close(self) -> None:
        if self.receiver:
            self.receiver.cancel()
        if self.socket:
            await self.socket.close()
        self.socket = None
