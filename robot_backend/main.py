"""Orchestrator: TRIGGER -> GREET -> LISTEN -> PROCESSING (STT -> rulebook
-> grounded or general LLM -> TTS + local playback) -> IDLE, looped forever.

Run with: python main.py
"""
from __future__ import annotations

import asyncio
import logging
import textwrap

import config
import llm
import mic
import rulebook as rulebook_mod
import stt
import tts
from ws_link import WSLink, WSLinkClosed, WSLinkError, WSLinkTimeout, WSServer

logger = logging.getLogger("robot_backend.main")

FALLBACK_ANSWER = "I'm not sure. Please ask a staff member."


def format_for_display(text: str, width: int = 12, max_lines: int = 5) -> str:
    """Cleans unicode chars and word-wraps text for the ESP32 TFT display in big font (textSize=2)."""
    text = text.replace('’', "'").replace('‘', "'").replace('“', '"').replace('”', '"').replace('—', '-').replace('–', '-')
    text = text.encode('ascii', 'ignore').decode('ascii')
    text = " ".join(text.split())
    lines = textwrap.wrap(text, width=width, break_long_words=True, break_on_hyphens=True)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
    return "|".join(lines)


async def run_interaction(link: WSLink, rulebook: rulebook_mod.Rulebook) -> None:
    """Runs exactly one full visitor interaction. Any protocol-level error
    is allowed to propagate to the caller, which logs it and returns the
    robot to waiting for the next trigger - one bad cycle should never take
    down the whole kiosk."""
    trigger_source = await link.wait_for_trigger()
    logger.info("Trigger received: %s", trigger_source)

    await link.send_command("GREET")
    await link.wait_for_status("GREETING_DONE", timeout=config.WS_COMMAND_TIMEOUT_S)

    # Send LISTEN, wait for ESP32 to signal it's ready, then record from
    # the laptop's own microphone.
    await link.send_command("LISTEN")
    await link.wait_for_status("LISTEN_READY", timeout=config.WS_COMMAND_TIMEOUT_S)
    pcm_audio = await asyncio.to_thread(mic.record)  # blocking mic capture, off the event loop
    await link.send_line("STATUS:RECORDING_DONE")  # tell ESP32 we're done
    logger.info("Recorded %.2fs of question audio from laptop mic",
                len(pcm_audio) / 2 / config.AUDIO_SAMPLE_RATE)

    await link.send_command("PROCESSING")

    question = await asyncio.to_thread(stt.transcribe, pcm_audio)
    if question:
        logger.info("Visitor asked: %s", question)
        print(f"\n==========================================")
        print(f"  You said: {question}")
        print(f"==========================================\n")
        # 'You said' is displayed only in the terminal, not on the TFT

    answer = await asyncio.to_thread(_generate_answer, question, rulebook)

    if not answer:
        await link.send_command("IDLE")
        await link.wait_for_status("IDLE", timeout=config.WS_COMMAND_TIMEOUT_S)
        return

    logger.info("Answer: %s", answer)
    print(f"\n==========================================")
    print(f"  Answer: {answer}")
    print(f"==========================================\n")

    clean_a = format_for_display(answer, width=12, max_lines=5)
    await link.send_command(f"DISPLAY_A:{clean_a}")

    # Play the answer locally (Piper synth + sounddevice playback), off the
    # event loop so the WebSocket connection stays responsive while it runs.
    await link.send_command("SPEAKING")
    played = await asyncio.to_thread(tts.synthesize_and_play, answer)
    if not played:
        logger.warning("TTS synthesis/playback produced no audio for: %r", answer)

    await link.send_command("IDLE")
    # ESP32 always ends runInteraction() with STATUS:IDLE - wait for it so
    # the connection is clean before we go back to wait_for_trigger().
    await link.wait_for_status("IDLE", timeout=config.WS_COMMAND_TIMEOUT_S)


def _generate_answer(question: str, rulebook: rulebook_mod.Rulebook) -> str:
    if not question:
        logger.warning("Empty transcript - nothing to answer")
        return ""

    matches = rulebook.match(question)
    try:
        if matches is not None:
            logger.info("Answer path: GROUNDED (rulebook match, top score=%d, id=%s)",
                        matches[0].score, matches[0].rule.id)
            return llm.answer_question(question, matches)
        logger.info("Answer path: GENERAL (no rulebook match for: %r)", question)
        return llm.answer_general(question)
    except llm.LLMError as e:
        logger.error("LLM call failed: %s", e)
        return FALLBACK_ANSWER


async def main_async() -> None:
    logging.basicConfig(
        level=getattr(logging, config.LOG_LEVEL.upper(), logging.INFO),
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )

    logger.info("Loading rulebook from %s", config.RULEBOOK_PATH)
    rulebook = rulebook_mod.Rulebook.load(config.RULEBOOK_PATH)

    server = WSServer(config.LAPTOP_WS_HOST, config.LAPTOP_WS_PORT,
                       command_timeout=config.WS_COMMAND_TIMEOUT_S)
    await server.start()

    while True:
        logger.info("Waiting for the ESP32 kiosk to connect...")
        link = await server.accept()
        try:
            await link.send_command("IDLE")  # sync ESP32 state on (re)connect
        except WSLinkError as e:
            logger.error("Failed to sync state on connect: %s - waiting for reconnect", e)
            continue

        logger.info("Connected. Waiting for a visitor...")
        while True:
            try:
                await run_interaction(link, rulebook)
            except WSLinkClosed as e:
                # The ESP32 dropped (WiFi hiccup, reboot, out of range) -
                # go back to accept() and wait for it to reconnect.
                logger.error("ESP32 disconnected: %s - waiting for reconnect", e)
                break
            except (WSLinkTimeout, WSLinkError) as e:
                # A single bad cycle (bad CRC, a stray line, a slow visitor)
                # - stay on the same connection and just wait for the next
                # trigger.
                logger.error("Protocol error, resuming on same connection: %s", e)


def main() -> None:
    asyncio.run(main_async())


if __name__ == "__main__":
    main()
