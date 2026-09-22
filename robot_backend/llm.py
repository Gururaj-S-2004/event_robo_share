"""LLM answer generation via Groq's OpenAI-compatible chat completions API.

This is the ONLY step in the pipeline allowed to call a paid/cloud service -
STT (stt.py), TTS (tts.py), and rulebook search (rulebook.py) are all local
and offline. Keep it that way: don't add other network calls here.

Two paths, chosen by main.py based on rulebook.Rulebook.match():
  - answer_question() - GROUNDED. Called on a rulebook match; phrases an
    answer from the matched facts only, never inventing anything.
  - answer_general() - UNGROUNDED fallback. Called on a rulebook miss; no
    event facts are attached, so it's free to chat/answer general-knowledge
    questions warmly, but is instructed not to invent event-specific facts.
"""
from __future__ import annotations

import logging

import requests

import config
from rulebook import RuleMatch

logger = logging.getLogger("robot_backend.llm")

GROQ_CHAT_URL = "https://api.groq.com/openai/v1/chat/completions"

GROUNDED_SYSTEM_PROMPT = (
    "You are a friendly event-kiosk robot for TECHTONIC 2026. "
    "Answer the visitor's question in ONE short, crisp, apt sentence (maximum 10 to 12 words). "
    "Your response is shown on a small screen and spoken aloud, so be direct and concise. "
    "No markdown, no lists, no unnecessary filler words. "
    "Only use the event facts provided below. If you don't know, say: "
    "'Please ask a staff member for help.' Never invent facts."
)

GENERAL_SYSTEM_PROMPT = (
    "You are a warm, friendly robot host at TECHTONIC 2026, chatting with a visitor. "
    "Answer in ONE short, spoken-style sentence (maximum 10 to 12 words) - your response "
    "is shown on a small screen and spoken aloud, so be direct and conversational, not robotic. "
    "No markdown, no lists, no unnecessary filler words. "
    "You have NOT been given any facts about this specific event, so: general-knowledge "
    "questions are fine to answer normally, but if the visitor is asking about event "
    "specifics (dates, locations, schedules, staff, rules, pricing, or anything else about "
    "THIS event), warmly say you don't have that detail and suggest asking a staff member. "
    "Never invent specific facts about this event."
)


class LLMError(Exception):
    pass


def _build_context(matches: list[RuleMatch]) -> str:
    if not matches:
        return "(no matching event facts found)"
    lines = [f"- {m.rule.answer}" for m in matches]
    return "\n".join(lines)


def _chat(system_prompt: str, user_prompt: str, temperature: float) -> str:
    if not config.GROQ_API_KEY:
        raise LLMError("GROQ_API_KEY is not set - add it to robot_backend/.env")

    payload = {
        "model": config.GROQ_MODEL,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "temperature": temperature,
        "max_tokens": 150,
    }
    headers = {"Authorization": f"Bearer {config.GROQ_API_KEY}"}

    try:
        resp = requests.post(GROQ_CHAT_URL, json=payload, headers=headers, timeout=15)
        resp.raise_for_status()
    except requests.RequestException as e:
        raise LLMError(f"Groq request failed: {e}") from e

    data = resp.json()
    try:
        return data["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError) as e:
        raise LLMError(f"Unexpected Groq response shape: {data}") from e


def answer_question(question: str, matches: list[RuleMatch]) -> str:
    """Grounded path: phrase an answer from rulebook facts. Call only with
    a non-empty `matches` (i.e. on a rulebook.Rulebook.match() hit) - an
    empty/no-fact call belongs to answer_general() instead."""
    context = _build_context(matches)
    user_prompt = (
        f"Event facts:\n{context}\n\n"
        f'Visitor asked: "{question}"\n\n'
        "Answer in ONE concise, apt sentence under 12 words."
    )
    text = _chat(GROUNDED_SYSTEM_PROMPT, user_prompt, temperature=0.3)
    logger.info("LLM grounded answer: %r", text)
    return text


def answer_general(question: str) -> str:
    """Ungrounded fallback for a rulebook miss: warm/conversational, may
    answer general-knowledge questions, but must not invent event-specific
    facts - see GENERAL_SYSTEM_PROMPT."""
    text = _chat(GENERAL_SYSTEM_PROMPT, question, temperature=0.5)
    logger.info("LLM general answer: %r", text)
    return text
