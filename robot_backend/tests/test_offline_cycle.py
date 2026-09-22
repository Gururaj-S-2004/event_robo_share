"""Full trigger -> greet -> listen -> processing -> speak(local) -> idle
cycle, run against a fake in-memory WebSocket transport (tests/fake_stream.py)
instead of real hardware/network. STT/TTS/LLM are monkeypatched so this test
is fast, fully offline, and doesn't need any ML model downloaded - it exists
purely to prove main.py and ws_link.py agree on the wire protocol with each
other (and, by inspection, with EventRobot.ino's matching comments).

Run with: pytest tests/test_offline_cycle.py -v
"""
from __future__ import annotations

import asyncio
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import llm  # noqa: E402
import main  # noqa: E402
import mic  # noqa: E402
import rulebook as rulebook_mod  # noqa: E402
import stt  # noqa: E402
import tts  # noqa: E402
from ws_link import WSLink  # noqa: E402
from tests.fake_stream import FakeWSPair  # noqa: E402


def _fake_pcm(n_samples: int, value: int = 1000) -> bytes:
    return struct.pack(f"<{n_samples}h", *([value] * n_samples))


async def _fake_device(device_link: WSLink, results: dict) -> None:
    """Plays the role of EventRobot.ino for one full interaction."""
    await device_link.send_line("TRIGGER:BUTTON")

    line = await device_link.readline(timeout=5)
    assert line == "COMMAND:GREET", line
    await device_link.send_line("STATUS:GREETING_DONE")

    line = await device_link.readline(timeout=5)
    assert line == "COMMAND:LISTEN", line
    await device_link.send_line("STATUS:LISTEN_READY")

    line = await device_link.readline(timeout=5)
    assert line == "STATUS:RECORDING_DONE", line

    line = await device_link.readline(timeout=10)
    if line == "COMMAND:PROCESSING":
        line = await device_link.readline(timeout=20)

    if line.startswith("COMMAND:DISPLAY_A:"):
        results["display_a"] = line[len("COMMAND:DISPLAY_A:"):]
        line = await device_link.readline(timeout=10)

    if line == "COMMAND:SPEAKING":
        results["speaking_hint"] = True
        line = await device_link.readline(timeout=20)

    assert line == "COMMAND:IDLE", line
    results["got_idle"] = True

    await device_link.send_line("STATUS:IDLE")


async def _run_scenario(rulebook: rulebook_mod.Rulebook) -> dict:
    pair = FakeWSPair()
    host_link = WSLink(pair.host, default_timeout=5)
    device_link = WSLink(pair.device, default_timeout=5)

    results: dict = {}
    device_task = asyncio.create_task(_fake_device(device_link, results))

    await main.run_interaction(host_link, rulebook)

    await asyncio.wait_for(device_task, timeout=10)
    return results


def test_full_interaction_cycle(monkeypatch):
    question_pcm = _fake_pcm(1600)  # 0.1s of fake "question" audio
    played_texts = []

    monkeypatch.setattr(mic, "record", lambda: question_pcm)
    monkeypatch.setattr(stt, "transcribe", lambda pcm: "where is registration")
    monkeypatch.setattr(
        llm, "answer_question", lambda question, matches: "Registration is at the front desk."
    )
    monkeypatch.setattr(
        llm, "answer_general",
        lambda question: pytest.fail("rulebook matched - answer_general should not be called"),
    )

    def fake_synthesize_and_play(text):
        played_texts.append(text)
        return b"\x00\x01" * 100

    monkeypatch.setattr(tts, "synthesize_and_play", fake_synthesize_and_play)

    rulebook = rulebook_mod.Rulebook(
        rules=[
            rulebook_mod.Rule(
                id="reg",
                keywords=["registration", "register"],
                answer="Registration is at the front desk.",
            )
        ]
    )

    results = asyncio.run(_run_scenario(rulebook))

    assert results.get("got_idle")
    assert results.get("speaking_hint")
    assert results.get("display_a") == "Registration|is at the|front desk."
    assert played_texts == ["Registration is at the front desk."]


def test_no_speech_goes_idle(monkeypatch):
    """Empty transcript -> backend should go straight to COMMAND:IDLE, with
    no DISPLAY_A/SPEAKING hint and no LLM call at all."""
    question_pcm = _fake_pcm(1600)

    monkeypatch.setattr(mic, "record", lambda: question_pcm)
    monkeypatch.setattr(stt, "transcribe", lambda pcm: "")
    monkeypatch.setattr(
        llm, "answer_question", lambda *a, **k: pytest.fail("LLM should not be called")
    )
    monkeypatch.setattr(
        llm, "answer_general", lambda *a, **k: pytest.fail("LLM should not be called")
    )

    rulebook = rulebook_mod.Rulebook(rules=[])

    results = asyncio.run(_run_scenario(rulebook))

    assert results.get("got_idle")
    assert "speaking_hint" not in results
    assert "display_a" not in results


def test_rulebook_miss_uses_general_answer(monkeypatch):
    """A question that doesn't match any rulebook entry must go through
    llm.answer_general() (the warm/ungrounded fallback), never
    llm.answer_question() (the grounded path) - see rulebook.Rulebook.match()
    and main._generate_answer()."""
    rulebook = rulebook_mod.Rulebook(
        rules=[
            rulebook_mod.Rule(
                id="reg",
                keywords=["registration", "register"],
                answer="Registration is at the front desk.",
            )
        ]
    )

    monkeypatch.setattr(
        llm, "answer_question",
        lambda *a, **k: pytest.fail("rulebook miss - grounded path should not be called"),
    )
    general_calls = []

    def fake_answer_general(question):
        general_calls.append(question)
        return "I don't have that detail, but a staff member will!"

    monkeypatch.setattr(llm, "answer_general", fake_answer_general)

    answer = main._generate_answer("what's the weather like today", rulebook)

    assert general_calls == ["what's the weather like today"]
    assert answer == "I don't have that detail, but a staff member will!"
