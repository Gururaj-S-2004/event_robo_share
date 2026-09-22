"""WebSocket transport for talking to the ESP32-S3 EventRobot over WiFi.

This module is the Python half of the wire protocol documented in the
WIRE PROTOCOL comment block at the top of ../EventRobot.ino. The two files
must always agree exactly - if you change one, change the other and re-run
tests/test_offline_cycle.py.

The laptop is the WebSocket *server* (this module); the ESP32 is the
WebSocket *client* that dials out to it. Only one kiosk connects at a time.

Protocol recap (each ASCII "line" below is one WebSocket TEXT frame - no
'\\n' needed, WebSocket messages are already framed):

  ESP32 -> laptop:
    STATUS:<text>
    TRIGGER:BUTTON | TRIGGER:PROXIMITY
    ERROR:<text>

  laptop -> ESP32:
    COMMAND:GREET | COMMAND:LISTEN | COMMAND:PROCESSING |
    COMMAND:DISPLAY_A:<text> | COMMAND:SPEAKING | COMMAND:IDLE

  laptop -> ESP32 (bare line, not COMMAND:-prefixed):
    STATUS:RECORDING_DONE

Mic capture and TTS playback both happen entirely on the laptop (mic.py,
tts.py) - no audio ever crosses this link in either direction today.
AUDIO_START:<len>:<crc32> / <len> raw PCM16LE mono 16kHz bytes (as one
BINARY frame) / AUDIO_END is kept below as a documented transport
primitive (read_audio_frame()/send_audio_frame()) for potential future
reuse, but nothing in this codebase calls it.
"""
from __future__ import annotations

import asyncio
import logging
import re
import zlib
from typing import Any, Optional

import websockets

logger = logging.getLogger("robot_backend.ws_link")

AUDIO_START_RE = re.compile(r"^AUDIO_START:(\d+):(\d+)$")

# Must match robot_backend/.env's LAPTOP_WS_HOST / LAPTOP_WS_PORT and the
# WS_HOST / WS_PORT #defines at the top of EventRobot.ino.
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 8765


class WSLinkError(Exception):
    """Any protocol-level failure: malformed frame, CRC mismatch, unexpected
    frame type, etc."""


class WSLinkTimeout(WSLinkError):
    """No message arrived before the deadline."""


class WSLinkClosed(WSLinkError):
    """The underlying WebSocket connection closed (the ESP32 disconnected).
    Callers should treat this as "wait for a reconnect", not a protocol bug."""


class WSLink:
    """Wraps one live WebSocket connection to the ESP32 kiosk."""

    def __init__(self, connection: Any, default_timeout: float = 20.0):
        self._conn = connection
        self._default_timeout = default_timeout

    async def close(self) -> None:
        await self._conn.close()

    # -- raw line I/O ---------------------------------------------------

    async def readline(self, timeout: Optional[float] = None) -> str:
        """Reads one TEXT-frame message (a "line" - no trailing newline,
        WebSocket frames are already delimited). Raises WSLinkTimeout if
        nothing arrives within `timeout` seconds, WSLinkClosed if the
        connection drops while waiting."""
        deadline = timeout if timeout is not None else self._default_timeout
        try:
            message = await asyncio.wait_for(self._conn.recv(), timeout=deadline)
        except asyncio.TimeoutError as e:
            raise WSLinkTimeout("no message received before timeout") from e
        except websockets.exceptions.ConnectionClosed as e:
            raise WSLinkClosed(f"connection closed: {e}") from e
        if isinstance(message, (bytes, bytearray)):
            raise WSLinkError(
                f"expected a text frame, got a {len(message)}-byte binary frame"
            )
        return message.rstrip("\r\n")

    async def send_line(self, text: str) -> None:
        try:
            await self._conn.send(text)
        except websockets.exceptions.ConnectionClosed as e:
            raise WSLinkClosed(f"connection closed: {e}") from e

    async def send_command(self, name: str) -> None:
        logger.debug("-> COMMAND:%s", name)
        await self.send_line(f"COMMAND:{name}")

    # -- high-level protocol helpers -------------------------------------

    async def wait_for_trigger(self, poll_timeout: float = 1.0) -> str:
        """Blocks (polling in poll_timeout-sized slices, forever) until a
        TRIGGER: line arrives. STATUS:/ERROR: lines seen while waiting are
        logged and ignored. Returns the trigger source, e.g. "BUTTON"."""
        while True:
            try:
                line = await self.readline(timeout=poll_timeout)
            except WSLinkTimeout:
                continue
            if not line:
                continue
            if line.startswith("TRIGGER:"):
                return line[len("TRIGGER:"):]
            if line.startswith("STATUS:"):
                logger.info("[ESP32] %s", line[len("STATUS:"):])
            elif line.startswith("ERROR:"):
                logger.warning("[ESP32] %s", line[len("ERROR:"):])
            else:
                logger.debug("[ESP32] unrecognized line while idle: %r", line)

    async def wait_for_status(self, expected: str, timeout: Optional[float] = None) -> None:
        """Blocks for a specific "STATUS:<expected>" line, raising
        WSLinkError if an ERROR: line or a non-matching STATUS arrives
        first (mirrors the ESP32's own strict COMMAND: expectations)."""
        line = await self.readline(timeout=timeout)
        if line == f"STATUS:{expected}":
            return
        if line.startswith("ERROR:"):
            raise WSLinkError(f"ESP32 reported error: {line[len('ERROR:'):]}")
        raise WSLinkError(f"expected STATUS:{expected}, got: {line!r}")

    # -- audio framing (documented but unused - see module docstring) -----

    async def read_audio_frame(self, timeout: Optional[float] = None) -> bytes:
        """Blocks for one AUDIO_START:<len>:<crc32> TEXT frame, one BINARY
        frame of exactly <len> bytes, and an AUDIO_END TEXT frame; returns
        the raw PCM16LE mono bytes. Not currently called anywhere."""
        header = await self.readline(timeout=timeout)
        m = AUDIO_START_RE.match(header)
        if not m:
            raise WSLinkError(f"expected AUDIO_START, got: {header!r}")
        length = int(m.group(1))
        expected_crc = int(m.group(2))

        deadline = timeout if timeout is not None else self._default_timeout
        try:
            payload = await asyncio.wait_for(self._conn.recv(), timeout=deadline)
        except asyncio.TimeoutError as e:
            raise WSLinkTimeout("timed out waiting for audio binary frame") from e
        except websockets.exceptions.ConnectionClosed as e:
            raise WSLinkClosed(f"connection closed: {e}") from e
        if not isinstance(payload, (bytes, bytearray)):
            raise WSLinkError("expected a binary frame for the audio payload, got text")
        if len(payload) != length:
            raise WSLinkError(
                f"audio frame length mismatch: header said {length}, got {len(payload)}"
            )

        end_line = await self.readline(timeout=2.0)
        if end_line != "AUDIO_END":
            raise WSLinkError(f"expected AUDIO_END, got: {end_line!r}")

        actual_crc = zlib.crc32(bytes(payload)) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise WSLinkError(
                f"audio CRC mismatch: expected {expected_crc}, got {actual_crc} "
                f"({len(payload)} bytes) - stream may be desynced"
            )
        return bytes(payload)

    async def send_audio_frame(self, pcm_bytes: bytes) -> None:
        """Sends one AUDIO_START:<len>:<crc32> TEXT frame, one BINARY frame
        with `pcm_bytes`, then an AUDIO_END TEXT frame. Not currently
        called anywhere - see module docstring."""
        crc = zlib.crc32(pcm_bytes) & 0xFFFFFFFF
        try:
            await self._conn.send(f"AUDIO_START:{len(pcm_bytes)}:{crc}")
            await self._conn.send(pcm_bytes)
            await self._conn.send("AUDIO_END")
        except websockets.exceptions.ConnectionClosed as e:
            raise WSLinkClosed(f"connection closed: {e}") from e


class WSServer:
    """Listens for the ESP32 kiosk to connect and hands back a ready-to-use
    WSLink. Only one kiosk is expected at a time; if a new connection
    arrives while a previous one is still open, the old one is closed."""

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT,
                 command_timeout: float = 20.0):
        self._host = host
        self._port = port
        self._command_timeout = command_timeout
        self._pending: "asyncio.Queue[Any]" = asyncio.Queue()
        self._active: Any = None
        self._server = None

    async def start(self) -> None:
        self._server = await websockets.serve(
            self._on_connect, self._host, self._port, ping_interval=20, ping_timeout=20
        )
        logger.info("WebSocket server listening on ws://%s:%d", self._host, self._port)

    async def _on_connect(self, websocket, *_args) -> None:
        # `*_args` absorbs the legacy `(websocket, path)` handler signature
        # some websockets versions still use, alongside the newer
        # single-argument `(websocket)` signature.
        logger.info("ESP32 connected from %s", websocket.remote_address)
        if self._active is not None and not self._active.closed:
            logger.warning("New ESP32 connection while a previous one was still open - closing the old one")
            await self._active.close()
        self._active = websocket
        await self._pending.put(websocket)
        # Keep this handler task alive for the connection's lifetime -
        # returning early would tear the socket down. WSLink methods use
        # `websocket` directly; this coroutine just waits for it to close.
        await websocket.wait_closed()
        logger.info("ESP32 disconnected")

    async def accept(self) -> WSLink:
        """Blocks until an ESP32 connects (or reconnects), returning a
        ready-to-use WSLink for that connection."""
        websocket = await self._pending.get()
        return WSLink(websocket, default_timeout=self._command_timeout)

    async def close(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
