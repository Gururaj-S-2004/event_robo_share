# EventRobot — AI Event-Kiosk Robot

A physical kiosk robot for events/exhibitions that greets visitors, listens
to their spoken questions, and answers out loud using a mix of **offline
speech recognition, a local knowledge base, a cloud LLM for phrasing, and
offline text-to-speech** — all running on an **ESP32-S3** (the "body":
sensors, mic-trigger, chirp speaker, servo, display) paired with a
**laptop** (the "brain": all the heavy processing, the microphone, and the
answer speakers) connected over **WiFi**. USB is power only.

## Why this exists

At an event/exhibition booth, staff repeatedly answer the same handful of
questions ("Where's registration?", "What time does it start?", "Where's
the main hall?"). EventRobot stands at the booth, waves and greets people
who walk up or press a button, listens to their question, and answers it
using facts you provide about *your specific event* — freeing up staff and
giving the booth a novelty/attraction factor. If a question isn't covered
by your facts, it still chats back warmly instead of going silent or
robotically refusing — see "Two answer paths" below.

It is deliberately built to be **mostly free to run**: transcription (STT)
and speech synthesis (TTS) both run fully offline/locally on the laptop.
The **only paid/cloud call** is a Groq LLM request per question, used to
phrase a natural-sounding answer — grounded strictly in the facts you
supply when your rulebook has a match, and explicitly told never to invent
event-specific facts when it doesn't.

## What has been built so far

- **`EventRobot.ino`** — firmware for the ESP32-S3: reads the button and
  ultrasonic sensor, drives the waving servo and TFT status screen, and
  plays a short locally-synthesized greeting chirp. Connects to WiFi in
  station mode and is the **WebSocket client** to the laptop's WebSocket
  server, with auto-reconnect (exponential backoff) on either WiFi or
  WebSocket drops. Runs a simple state machine (`IDLE → GREETING →
  LISTENING → WAITING_RESPONSE → IDLE`) and talks to the laptop over that
  WebSocket link using a small custom text/binary-frame protocol. No audio
  ever crosses the link in either direction — the laptop's own mic and
  speakers handle both ends of that.
- **`robot_backend/`** — the Python program that runs on the laptop:
  - `main.py` — orchestrates one full visitor interaction end-to-end, looped
    forever.
  - `ws_link.py` — the Python half of the wire protocol; runs the
    WebSocket server the ESP32 connects to (must always stay in sync with
    the `WIRE PROTOCOL` comment block at the top of `EventRobot.ino`).
  - `mic.py` — records the visitor's question from the laptop's own
    microphone (`sounddevice`).
  - `stt.py` — offline speech-to-text using **faster-whisper**.
  - `rulebook.py` — local keyword search over `data/rulebook.json` (your
    event facts), used to ground the LLM so it doesn't hallucinate, and to
    signal an explicit **miss** when nothing matches.
  - `llm.py` — sends the question to **Groq** (OpenAI-compatible chat
    completions API) and gets back a short spoken-style answer: a
    *grounded* path (facts + question) on a rulebook match, and a *general*,
    warm/friendly ungrounded path on a rulebook miss. This is the only
    network/paid call in the whole pipeline.
  - `tts.py` — offline text-to-speech using **Piper**, played immediately
    on the laptop's own speakers (`sounddevice`).
  - `config.py` — all settings, loaded from a local `.env` file.
  - `data/rulebook.json` — the editable "knowledge base" of event facts.
  - `tests/` — an offline test (`test_offline_cycle.py`) that replays a
    full trigger→greet→listen→process→speak(local)→idle cycle against a
    fake, in-memory WebSocket transport (no hardware or real network
    needed) to catch protocol mismatches between the firmware and the
    backend before they reach a live demo.

## How it works, end to end

```
 Visitor walks up / presses button
              │
              ▼
   ESP32 detects trigger (button or ultrasonic proximity)
              │  TRIGGER:BUTTON / TRIGGER:PROXIMITY  (WebSocket text frame)
              ▼
   Laptop (main.py) sends COMMAND:GREET
              │
              ▼
   ESP32 waves servo, plays a LOCAL greeting chirp (I2S, no laptop
   round-trip), shows "Hello!" on the TFT
              │  STATUS:GREETING_DONE
              ▼
   Laptop sends COMMAND:LISTEN
              │
              ▼
   ESP32 signals STATUS:LISTEN_READY, shows "Listening..." on the TFT
              │
              ▼
   Laptop records ~5s from ITS OWN microphone (mic.py/sounddevice),
   sends STATUS:RECORDING_DONE  (no audio crosses the WebSocket link)
              │
              ▼
   Laptop: faster-whisper transcribes the audio → text question
              │
              ▼
   Laptop: rulebook.py keyword-matches the question against your event
   facts (data/rulebook.json) — an explicit MATCH or MISS signal
              │
        ┌─────┴─────┐
        ▼           ▼
     MATCH        MISS
        │           │
        ▼           ▼
   llm.py: grounded   llm.py: warm/general answer, no event
   answer from the    facts attached — free to chat, but told
   matched facts       never to invent event-specific details
        │           │
        └─────┬─────┘
              ▼
   Laptop sends COMMAND:DISPLAY_A:<answer> (TFT shows the answer),
   then COMMAND:SPEAKING (TFT shows "Speaking...")
              │
              ▼
   Laptop: tts.py (Piper) synthesizes the answer, offline, and plays
   it immediately on the LAPTOP'S OWN speakers (sounddevice)
              │
              ▼
   Laptop sends COMMAND:IDLE
              │  STATUS:IDLE
              ▼
   Back to IDLE, ready for the next visitor
```

The laptop is the only side that talks to the internet (and only for the
Groq LLM call). The ESP32 connects to your event's WiFi purely to reach the
laptop's WebSocket server on the local network — it never talks to any
outside/cloud service directly, and no audio (question or answer) ever
crosses that WebSocket link; the laptop's own mic and speakers handle both
ends of the audio path.

### Two answer paths: grounded vs. general

`rulebook.py` gives an explicit **match**/**miss** signal (never a silent
"pick the weakest match"). On a match, `llm.py`'s grounded path phrases an
answer strictly from your rulebook facts. On a miss, `llm.py`'s
`answer_general()` path is used instead: no event facts are attached, the
model is free to chat or answer general-knowledge questions in a warm,
conversational tone, but it's explicitly instructed never to invent
specifics about *this* event — it says so and points the visitor to a
staff member instead. `main.py` logs which path was taken per interaction,
so you can see your rulebook's coverage gaps over time.

## Hardware — bill of materials

| Component | Purpose | Notes |
|---|---|---|
| ESP32-S3 DevKit board | Main controller ("body") | Any ESP32-S3 dev board with enough exposed GPIO and WiFi; avoids strapping pins 0/3/45/46 and native-USB pins 19/20 |
| Push button | Manual trigger | Any momentary NO push button |
| HC-SR04 ultrasonic sensor | Proximity trigger (auto-greet when someone approaches) | 5V logic — needs a voltage divider on ECHO, see wiring below |
| SG90 (or similar) hobby servo | Waving "hand" gesture | Standard 3-wire hobby servo, 50Hz PWM |
| 1.8" SPI TFT display (ST7735, 128×160) | Status text ("Ready", "Listening...", "Speaking...", etc.) | See wiring below |
| MAX98357A I2S class-D amplifier + small 4–8Ω speaker | **Optional** — only needed if you want the greeting chirp through a real speaker | I2S digital audio in, speaker out. The spoken answer is never played here — it plays on the laptop's own speakers |
| USB cable (data-capable, for flashing) | Power only at runtime | Also used for flashing and the local debug Serial Monitor; no robot↔laptop protocol data crosses it anymore |
| WiFi network / hotspot | Connects the ESP32 and the laptop | Both devices must be on the same LAN/hotspot, laptop reachable at a stable local IP |
| Laptop / PC | Runs the Python backend (mic, STT, LLM call, TTS, speakers) | Windows, on the same WiFi network as the ESP32; see software setup below |

## Circuit / wiring connections

All pin numbers below are ESP32-S3 GPIO numbers, exactly as defined at the
top of `EventRobot.ino` (`#define PIN_...`). If you use a different board
layout, only the `#define` block needs to change — nothing else in the
firmware.

### Trigger inputs

| Signal | ESP32-S3 GPIO | Wiring |
|---|---|---|
| Push button | GPIO 4 | One leg → GPIO 4, other leg → GND. Uses the internal pull-up (`INPUT_PULLUP`) — no external resistor needed. Pressed = LOW. |
| HC-SR04 `TRIG` | GPIO 5 | Direct connection (TRIG is a 3.3V-compatible input on the sensor) |
| HC-SR04 `ECHO` | GPIO 6 | **Do not connect directly** — ECHO outputs 5V and the ESP32 is 3.3V-only on its GPIOs. Use a resistor divider, e.g. 1kΩ (ECHO→node) + 2kΩ (node→GND), and feed the midpoint node into GPIO 6. |
| HC-SR04 `VCC` | 5V rail | From the ESP32 board's 5V pin (or an external 5V supply) |
| HC-SR04 `GND` | GND | Common ground with the ESP32 |

### Actuators

| Signal | ESP32-S3 GPIO | Wiring |
|---|---|---|
| Servo signal | GPIO 7 | Servo signal wire → GPIO 7. Servo `+`/red → 5V, servo `-`/brown or black → GND. **Power the servo from a 5V source that can supply its stall current** (not directly from the ESP32's onboard 3.3V regulator) — share ground with the ESP32. |

### 1.8" TFT SPI Display (ST7735 128x160 V1.1)

| Signal | ESP32-S3 Pin | Wiring |
|---|---|---|
| VCC | 3.3V (or 5V) | Power pin (3.3V recommended) |
| GND | GND | Common ground |
| CS | GPIO 10 | To TFT `CS` (Chip Select) |
| RESET / RES | GPIO 8 | To TFT `RESET` / `RES` |
| A0 / DC | GPIO 9 | To TFT `A0` / `DC` (Data/Command) |
| SDA / MOSI | GPIO 11 | To TFT `SDA` / `MOSI` / `DIN` (Data In) |
| SCL / SCK | GPIO 12 | To TFT `SCL` / `SCK` / `CLK` (SPI Clock) |
| LED / BLK | 3.3V | To TFT `LED` / `BLK` (Backlight power - must be wired to light up the display) |

### Microphone — none on the ESP32

There is no microphone on the robot itself. The visitor's question is
captured by the **laptop's own microphone** (`robot_backend/mic.py`, via
`sounddevice`) once the ESP32 signals `STATUS:LISTEN_READY` — nothing to
wire here.

### Speaker amplifier — MAX98357A (optional, greeting chirp only)

Only needed if you want the greeting chirp played through a real speaker
instead of skipping it silently. The spoken answer is **never** played
here — it plays on the laptop's own speakers (`robot_backend/tts.py`).

| Signal | ESP32-S3 GPIO | Wiring |
|---|---|---|
| BCLK | GPIO 1 | To MAX98357A `BCLK` |
| LRC / WS | GPIO 2 | To MAX98357A `LRC` |
| DIN | GPIO 38 | To MAX98357A `DIN` |
| SD | — | Tie high (always enabled) or to a spare GPIO if you want software mute control |
| Speaker out | — | MAX98357A's `+`/`-` speaker terminals → your 4–8Ω speaker |
| VIN | 5V (or 3–5.5V per datasheet) | Power rail |
| GND | GND | Common ground |

### WiFi / WebSocket configuration

No wiring here — just firmware `#define`s, at the top of `EventRobot.ino`:

| Setting | Meaning |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Your event's WiFi network credentials |
| `WS_HOST` | The laptop's LAN IP (from `ipconfig` on the laptop) — must match `LAPTOP_WS_HOST`'s network reachability, not `0.0.0.0` itself |
| `WS_PORT` | Must match `robot_backend/.env`'s `LAPTOP_WS_PORT` (default `8765`) |

### Power notes

- Share a **common ground** across every module (ESP32, HC-SR04, servo,
  MAX98357A, TFT) — this is the single most common source of "flaky
  sensor" or "no audio" bugs.
- The servo and the (optional) amplifier can both draw meaningful current —
  if you see brownouts/resets when the servo moves or the chirp plays,
  power them from a dedicated 5V supply rather than solely through the
  ESP32 board's onboard regulator, and make sure that supply's ground is
  tied back to the ESP32's ground.
- **USB is power only at runtime.** All robot↔laptop communication now
  happens over WiFi/WebSocket; the USB cable is only needed for flashing
  the firmware and, optionally, watching the local debug Serial Monitor.

## Software setup — from a blank laptop to a working demo

### 1. Flash the ESP32-S3 firmware

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) (2.x).
2. In **Boards Manager**, install the **esp32** board package (Espressif
   Systems), then select an **ESP32S3 Dev Module** board.
3. Install these libraries via **Library Manager**:
   - `Adafruit GFX Library`
   - `Adafruit ST7735 and ST7789 Library`
   - `ESP32Servo`
   - `WebSockets` by Markus Sattler (Links2004/arduinoWebSockets)
   - (the I2S driver used, `driver/i2s.h`, and `WiFi.h` ship with the
     ESP32 core — no separate install needed)
4. Wire up the hardware exactly as in the tables above.
5. Edit the `#define`s at the top of `EventRobot.ino`: `WIFI_SSID`,
   `WIFI_PASSWORD`, `WS_HOST` (the laptop's LAN IP — run `ipconfig` on the
   laptop once it's on the same network), and `WS_PORT` (must match
   `robot_backend/.env`'s `LAPTOP_WS_PORT`, default `8765`).
6. Open `EventRobot.ino`, select the correct COM port, and click **Upload**.
7. Optionally open the Serial Monitor at **115200 baud** for local debug
   output (WiFi/WebSocket connect attempts, `STATUS:RECONNECTING` /
   `STATUS:READY`, etc.) — this is debug-only now, not part of the
   robot↔laptop protocol. The TFT should show "Starting / Connecting to
   WiFi..." then "Ready / Press button or stand close" once it reaches the
   laptop.

### 2. Set up the laptop backend

All commands below are run from the `robot_backend/` folder.

```
py -3.11 -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
```

> `faster-whisper` (via `ctranslate2`) and `piper-tts` don't reliably have
> prebuilt wheels for brand-new Python releases — if your default `python`
> is newer than 3.11/3.12, install with `py -3.11` explicitly as above.

Copy the environment template and fill it in:

```
copy .env.example .env
```

Then edit `.env`:

- `LAPTOP_WS_HOST` / `LAPTOP_WS_PORT` — the WebSocket server this backend
  runs. `0.0.0.0` (the default) listens on every network interface; the
  ESP32 firmware's `WS_HOST` #define needs this laptop's actual LAN IP
  (`ipconfig`), not `0.0.0.0`. Port must match the firmware's `WS_PORT`.
- `GROQ_API_KEY` — get a free key from [console.groq.com](https://console.groq.com)
  and paste it here. **Only in `.env`, never in a file you'd commit** —
  make sure `.env` is gitignored.
- `PIPER_MODEL_PATH` — path to a downloaded Piper voice `.onnx` file (next
  step).

WiFi credentials (`WIFI_SSID` / `WIFI_PASSWORD`) are set on the
**firmware** side, in `EventRobot.ino` — this backend has none of its own.

### 3. Download an offline TTS voice (Piper)

Piper voices are published on Hugging Face (`rhasspy/piper-voices`). For
`en_US-lessac-medium`:

```
mkdir voices
curl -L -o voices/en_US-lessac-medium.onnx ^
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx
curl -L -o voices/en_US-lessac-medium.onnx.json ^
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json
```

Both the `.onnx` file and its matching `.onnx.json` config must sit in the
same folder.

`faster-whisper`'s STT model (`small.en` by default, set via
`WHISPER_MODEL_SIZE` in `.env`) downloads itself automatically the first
time `main.py` runs — no manual step needed, just make sure the laptop has
internet on that first run.

### 4. Fill in your event's facts

Edit `robot_backend/data/rulebook.json` and replace every `TODO` field:

```json
{
  "id": "unique-id",
  "keywords": ["words", "a visitor", "might", "say"],
  "question_hint": "example question this rule answers (reference only, not used by code)",
  "answer": "the exact sentence(s) the robot should speak"
}
```

Keep `keywords` generous (synonyms, informal phrasing) — matching is a
simple keyword-overlap search, there's no fuzzy matching or embeddings
involved.

### 5. Connect the hardware and run it

1. Make sure the laptop and the ESP32-S3 are on the **same WiFi
   network/hotspot**, and that the laptop's firewall allows inbound
   connections on `LAPTOP_WS_PORT` (Windows may prompt the first time
   `python main.py` opens the port — allow it on Private networks).
2. Power the ESP32-S3 over USB (or any 5V supply) — no data connection to
   the laptop is needed over USB anymore.
3. From `robot_backend/`, with the venv active:

```
python main.py
```

You should see a log line for the WebSocket server starting, then
`Waiting for the ESP32 kiosk to connect...`, then `Connected. Waiting for
a visitor...` once the board reaches it. Press the button (or walk within
~60cm of the ultrasonic sensor) to trigger a full interaction.

### 6. Test the software without any hardware plugged in

```
pip install pytest
pytest tests/ -v
```

`tests/test_offline_cycle.py` runs a full trigger → greet → listen →
processing → speak(local)→ idle cycle against an in-memory fake WebSocket
transport, with STT/TTS/LLM monkeypatched out, plus a dedicated test for
the rulebook-miss → `llm.answer_general()` path. It exists to catch
wire-protocol mismatches between `main.py` / `ws_link.py` and
`EventRobot.ino` without needing a board or a real network socket —
useful for developing away from the physical kiosk.

## The wire protocol (laptop ↔ ESP32)

WebSocket text/binary frames over WiFi, laptop as server / ESP32 as
client. Documented in full, and kept in sync, in two places: the `WIRE
PROTOCOL` comment block at the top of `EventRobot.ino`, and the module
docstring in `robot_backend/ws_link.py`.

**ESP32 → laptop (text frames):**
- `STATUS:<text>` — informational, laptop just logs it (`READY`,
  `LISTEN_READY`, `GREETING_DONE`, `IDLE`)
- `TRIGGER:BUTTON` / `TRIGGER:PROXIMITY` — a visitor triggered an interaction
- `ERROR:<text>` — something went wrong on the ESP32 side

**Laptop → ESP32 (text frames):**
- `COMMAND:GREET` — play the wave + LOCAL greeting chirp
- `COMMAND:LISTEN` — ESP32 signals `LISTEN_READY`; laptop records from its
  own mic and replies with `STATUS:RECORDING_DONE`
- `COMMAND:PROCESSING` / `COMMAND:DISPLAY_A:<text>` / `COMMAND:SPEAKING` —
  status hints only (TFT text), no audio involved
- `COMMAND:IDLE` — return to idle

No audio (question or answer) crosses this link in either direction — the
laptop's own mic and speakers handle both ends. A binary-frame
`AUDIO_START`/`AUDIO_END` primitive is still defined in `ws_link.py` for
potential future reuse, but nothing calls it today.

If you ever change one side of this protocol, change the other and re-run
`pytest tests/ -v` before testing on real hardware.

## Known fragile points / things to double-check before a live event

- **WiFi/WebSocket reconnects use exponential backoff (1s → 30s cap)** —
  if the ESP32 loses the laptop's WiFi or the WebSocket drops mid-event, it
  retries automatically and shows "Reconnecting..." on the TFT; the
  laptop's `main.py` waits for the reconnect and resends `COMMAND:IDLE` to
  resync state. A long outage means a growing wait between retries (up to
  30s) rather than a hammering retry loop.
- **Fixed ~5-second listening window** — `MIC_RECORD_SECONDS` in
  `robot_backend/.env` (laptop-side, since the mic is the laptop's own) is
  a fixed capture window, not silence-detected. A visitor who talks past
  it gets truncated. Raise it there if that's a problem live — no reflash
  needed.
- **Groq model name** — `GROQ_MODEL` in `.env.example` is a best-guess
  default (`llama-3.1-8b-instant`); Groq's available model list changes
  over time. Confirm the model is still served before the event, since an
  invalid model name only surfaces at request time as an `LLMError`, not at
  startup.
- **The `answer_general()` fallback still calls Groq** — a rulebook miss
  isn't free; it's the same LLM call, just without event facts attached.
  Keep an eye on `llm.py`'s logged GROUNDED/GENERAL path per interaction to
  gauge your rulebook's coverage and Groq usage.

## Project structure

```
event robo/
├── EventRobot.ino              ESP32-S3 firmware (WiFi + WebSocket client)
└── robot_backend/              Laptop-side Python backend
    ├── main.py                 Orchestrator loop (asyncio)
    ├── ws_link.py               Wire-protocol transport (WebSocket server)
    ├── mic.py                  Laptop mic capture (sounddevice)
    ├── stt.py                  Offline speech-to-text (faster-whisper)
    ├── rulebook.py              Local keyword search over event facts
    ├── llm.py                  Groq LLM calls: grounded + general fallback
    ├── tts.py                  Offline text-to-speech (Piper) + local playback
    ├── config.py                Settings, loaded from .env
    ├── .env.example             Environment variable template
    ├── data/
    │   └── rulebook.json        Your event's facts (edit this!)
    ├── voices/                  Downloaded Piper .onnx voice models
    └── tests/
        ├── fake_stream.py       In-memory fake WebSocket transport for testing
        └── test_offline_cycle.py  Full-cycle test, no hardware needed
```
