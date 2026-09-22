"""An in-memory, asyncio-based duplex transport pair standing in for a real
WebSocket connection, so ws_link.WSLink can be exercised without any real
network socket or hardware - see test_offline_cycle.py.
"""
from __future__ import annotations

import asyncio
from typing import Union

Message = Union[str, bytes]


class _HalfDuplex:
    """One direction of a duplex pipe: messages sent on the 'in' side show
    up (in order) on the 'out' side via an asyncio.Queue - mirrors a
    WebSocket connection's send()/recv() shape closely enough for WSLink."""

    def __init__(self):
        self._q: "asyncio.Queue[Message]" = asyncio.Queue()

    async def send(self, data: Message) -> None:
        await self._q.put(data)

    async def recv(self) -> Message:
        return await self._q.get()


class FakeWSPair:
    """Two WSLink-compatible endpoints, `device` and `host`, wired so that
    sends on one side are recvs on the other - like a null-modem cable
    between the (simulated) ESP32 and the backend, but over asyncio queues
    instead of a real socket."""

    def __init__(self):
        device_to_host = _HalfDuplex()
        host_to_device = _HalfDuplex()

        self.device = _Endpoint(recv_from=device_to_host, send_to=host_to_device)
        self.host = _Endpoint(recv_from=host_to_device, send_to=device_to_host)


class _Endpoint:
    def __init__(self, recv_from: _HalfDuplex, send_to: _HalfDuplex):
        self._recv_from = recv_from
        self._send_to = send_to

    async def recv(self) -> Message:
        return await self._recv_from.recv()

    async def send(self, data: Message) -> None:
        await self._send_to.send(data)

    async def close(self) -> None:
        pass
