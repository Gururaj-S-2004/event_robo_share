# EventRobot backend

Runs on the laptop, talks to the ESP32-S3 kiosk over WiFi (this backend is
the WebSocket *server*, the ESP32 is the WebSocket *client*). Orchestrates:
wait for TRIGGER -> COMMAND:GREET -> COMMAND:LISTEN (record from the
laptop's own mic) -> COMMAND:PROCESSING (offline STT -> local rulebook
match/miss -> grounded or general Groq LLM answer -> offline TTS) ->
COMMAND:SPEAKING (plays the answer on the laptop's own speakers) ->
COMMAND:IDLE. USB is power only - no protocol data crosses it.

The exact frame framing is documented in two places that must always
agree: the `WIRE PROTOCOL` comment block at the top of `../EventRobot.ino`,
and the module docstring in `ws_link.py`.

Everything is offline/free except the LLM call (`llm.py`, via Groq's API) -
STT is `faster-whisper`, TTS is `Piper`, playback and mic capture are
`sounddevice`, and rulebook lookup is local keyword search over
`data/rulebook.json`.

## Two answer paths

`rulebook.py`'s `Rulebook.match()` returns an explicit `None` on a miss
(never a silently-weak match). `main.py` branches on that:
- **Match** -> `llm.answer_question()` - grounded, phrases an answer
  strictly from the matched facts.
- **Miss** -> `llm.answer_general()` - no event facts attached; warm and
  conversational, free to answer general-knowledge questions, but
  instructed to never invent facts about *this* event and to point the
  visitor to a staff member instead.

`main.py` logs which path was taken per interaction (`Answer path:
GROUNDED` / `Answer path: GENERAL`) so you can spot rulebook coverage gaps.

## Setup

```
py -3.11 -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
```

`faster-whisper` (via `ctranslate2`) and `piper-tts` don't reliably have
prebuilt wheels for brand-new Python releases - if your default `python`
is newer than 3.11/3.12, install with `py -3.11` explicitly, as above.

Copy the environment template and fill it in:

```
copy .env.example .env
```

Then edit `.env`:
- `LAPTOP_WS_HOST` / `LAPTOP_WS_PORT` - the WebSocket server this backend
  runs (default `0.0.0.0:8765`, listening on every local interface). The
  ESP32 firmware's `WS_HOST` #define needs this laptop's actual LAN IP
  (`ipconfig`), not `0.0.0.0`; `WS_PORT` must match `LAPTOP_WS_PORT`.
- `GROQ_API_KEY` - paste your Groq API key here. **Only in `.env`, never in
  a file you'd commit** - `.env` should be gitignored.
- `PIPER_MODEL_PATH` - path to a downloaded Piper voice `.onnx` file (see
  below).

WiFi credentials (`WIFI_SSID` / `WIFI_PASSWORD`) live on the **firmware**
side, in `../EventRobot.ino` - this backend has none of its own.

## Downloading a Piper voice

Piper voices are published on Hugging Face
(`rhasspy/piper-voices`). For `en_US-lessac-medium`:

```
mkdir voices
curl -L -o voices/en_US-lessac-medium.onnx ^
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx
curl -L -o voices/en_US-lessac-medium.onnx.json ^
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json
```

Both the `.onnx` and its matching `.onnx.json` config file are required in
the same directory.

## Filling in the rulebook

Edit `data/rulebook.json`. Every `TODO` field needs replacing:

```json
{
  "id": "unique-id",
  "keywords": ["words", "a visitor", "might", "say"],
  "question_hint": "example question this rule answers (not used by code, just for your own reference)",
  "answer": "the exact sentence(s) the robot should speak"
}
```

`rulebook.py` does simple keyword-overlap search - keep `keywords` lists
generous (include synonyms, informal phrasing) since there's no fuzzy
matching or embeddings involved. Anything that doesn't score above
threshold falls through to `llm.answer_general()` instead of a grounded
answer - see "Two answer paths" above.

## Running

```
python main.py
```

Make sure the laptop's firewall allows inbound connections on
`LAPTOP_WS_PORT` (Windows will usually prompt the first time - allow it on
Private networks), and that the ESP32 is on the same WiFi network.

## Testing without hardware

```
pip install pytest
pytest tests/ -v
```

`tests/test_offline_cycle.py` runs a full trigger -> greet -> listen ->
processing -> speak(local) -> idle cycle against an in-memory fake
WebSocket transport (`tests/fake_stream.py`), with STT/TTS/LLM
monkeypatched out, plus a dedicated test asserting a rulebook miss calls
`llm.answer_general()` and never the grounded path. It exists to catch
wire-protocol desyncs between `main.py`/`ws_link.py` and `EventRobot.ino`
without needing a board or a real network socket.

## Fragile points / things to double-check before a live demo

- **WiFi/WebSocket reconnects use exponential backoff (1s -> 30s cap)** -
  on a drop, the ESP32 retries automatically and shows "Reconnecting..."
  on the TFT; `main.py` waits for the reconnect (`WSServer.accept()`) and
  resends `COMMAND:IDLE` to resync state once it's back. Don't restart
  `main.py` to "fix" a stuck interaction if the board itself is fine - it
  will just wait for the same reconnect the board is already doing.
- **Fixed ~5-second listening window**: `MIC_RECORD_SECONDS` in `.env` is a
  fixed capture window, not silence-detected. A visitor who talks past it
  gets truncated - raise it in `.env` if that's a problem live (no
  reflash needed, since the mic is the laptop's own).
- **Groq model name**: `GROQ_MODEL` in `.env.example` is a best-guess
  default (`llama-3.1-8b-instant`) - Groq's available model list changes;
  confirm the model is still served before the event, since an invalid
  model name will surface at request time as `LLMError`, not at startup.
- **`answer_general()` still costs a Groq call**: a rulebook miss isn't
  free, it's the same LLM request minus the event facts. Watch the logged
  GROUNDED/GENERAL path per interaction to gauge rulebook coverage and API
  usage.
