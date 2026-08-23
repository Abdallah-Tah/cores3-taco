from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
import subprocess
import tempfile
from collections.abc import Awaitable, Callable

from websockets.asyncio.client import connect

logger = logging.getLogger("taco-hub.realtime")
SUPPORTED_VOICES = {"cedar", "ash", "echo", "verse", "edge"}

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
        self.conversation_active = False
        self.voice = os.getenv("TACO_REALTIME_VOICE", "cedar")
        self.turn_audio_bytes = 0
        self._downlink_audio = bytearray()
        self._text_response: list[str] = []

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
                        "output_modalities": ["text"] if self.voice == "edge" else ["audio"],
                        "instructions": (
                            "You are Taco, a warm, clever, concise home companion. "
                            "Speak naturally, answer quickly, and keep most replies under "
                            "three sentences unless the user asks for detail."
                        ),
                        "audio": {
                            "input": {
                                "format": {"type": "audio/pcm", "rate": 24000},
                                "turn_detection": {"type": "semantic_vad"},
                            },
                            **({} if self.voice == "edge" else {"output": {
                                "format": {"type": "audio/pcm", "rate": 24000},
                                "voice": self.voice,
                            }}),
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
        self.turn_audio_bytes = 0
        await self.send_json({"type": "voice_state", "state": "listening"})

    async def start_conversation(self) -> None:
        await self.connect()
        self.conversation_active = True
        self.turn_audio_bytes = 0
        await self.socket.send(json.dumps({"type": "input_audio_buffer.clear"}))
        await self.send_json({"type": "voice_state", "state": "listening"})

    async def stop_conversation(self) -> None:
        self.conversation_active = False
        self.speaking = False
        if self.socket is not None:
            await self.socket.send(json.dumps({"type": "input_audio_buffer.clear"}))
        self.turn_audio_bytes = 0
        await self.send_json({"type": "voice_state", "state": "idle"})

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
        self.turn_audio_bytes += len(pcm16)

    async def finish_turn(self) -> None:
        if self.socket is None:
            return
        if self.turn_audio_bytes < 4800:
            await self.cancel_turn()
            await self.send_json(
                {"type": "voice_state", "state": "error", "message": "Hold longer"}
            )
            return
        await self.socket.send(json.dumps({"type": "input_audio_buffer.commit"}))
        await self.socket.send(json.dumps({"type": "response.create"}))
        self.turn_audio_bytes = 0
        await self.send_json({"type": "voice_state", "state": "thinking"})

    async def cancel_turn(self) -> None:
        if self.socket is not None:
            await self.socket.send(json.dumps({"type": "input_audio_buffer.clear"}))
        self.turn_audio_bytes = 0
        await self.send_json({"type": "voice_state", "state": "idle"})

    async def _receive(self) -> None:
        try:
            async for raw in self.socket:
                event = json.loads(raw)
                event_type = event.get("type", "")
                if event_type == "input_audio_buffer.speech_started":
                    await self.send_json({"type": "voice_state", "state": "listening"})
                elif event_type == "input_audio_buffer.speech_stopped":
                    await self.send_json({"type": "voice_state", "state": "thinking"})
                elif event_type == "response.output_text.delta":
                    self._text_response.append(event.get("delta", ""))
                elif event_type == "response.output_audio.delta":
                    if not self.speaking:
                        self.speaking = True
                        await self.send_json({"type": "voice_state", "state": "speaking"})
                    self._downlink_audio.extend(base64.b64decode(event.get("delta", "")))
                    while len(self._downlink_audio) >= 4800:
                        await self.send_bytes(bytes(self._downlink_audio[:4800]))
                        del self._downlink_audio[:4800]
                        await asyncio.sleep(0.075)
                elif event_type == "response.done":
                    if self.voice == "edge" and self._text_response:
                        text = "".join(self._text_response).strip()
                        self._text_response.clear()
                        audio = await self._edge_audio(text)
                        if audio:
                            await self.send_json({"type": "voice_state", "state": "speaking"})
                            for offset in range(0, len(audio), 4800):
                                await self.send_bytes(audio[offset : offset + 4800])
                                await asyncio.sleep(0.075)
                    if self._downlink_audio:
                        await self.send_bytes(bytes(self._downlink_audio))
                        self._downlink_audio.clear()
                        await asyncio.sleep(0.075)
                    self.speaking = False
                    await self.send_json(
                        {
                            "type": "voice_state",
                            "state": "listening" if self.conversation_active else "idle",
                        }
                    )
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
        self.conversation_active = False

    async def _edge_audio(self, text: str) -> bytes:
        """Synthesize Edge TTS to Taco's 24 kHz mono PCM wire format."""
        try:
            import edge_tts

            voice = os.getenv("TACO_EDGE_VOICE", "en-US-GuyNeural")
            with tempfile.NamedTemporaryFile(suffix=".mp3") as media:
                await edge_tts.Communicate(text, voice).save(media.name)
                result = await asyncio.to_thread(
                    subprocess.run,
                    ["ffmpeg", "-v", "error", "-i", media.name, "-f", "s16le",
                     "-ac", "1", "-ar", "24000", "pipe:1"],
                    check=True, capture_output=True,
                )
                return result.stdout
        except Exception:
            logger.exception("Edge TTS synthesis failed")
            return b""
