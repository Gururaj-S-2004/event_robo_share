#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ESP32Servo.h>
#include <driver/i2s.h>
#include <math.h>
#include <WiFi.h>
#include <WebSocketsClient.h>

// ============================================================================
// WIFI / WEBSOCKET SETTINGS - edit these for your event's network.
// ============================================================================
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// The laptop's WebSocket server - must be the laptop's actual LAN IP (check
// `ipconfig` on Windows), NOT 0.0.0.0, since the ESP32 dials out to it.
// Port must match robot_backend/.env's LAPTOP_WS_PORT.
#define WS_HOST "192.168.1.100"
#define WS_PORT 8765
#define WS_PATH "/"

// ============================================================================
// WIRE PROTOCOL (WiFi + WebSocket, matches robot_backend/ws_link.py)
// ----------------------------------------------------------------------------
// Transport: the ESP32-S3 connects to WiFi in station mode (WIFI_SSID /
// WIFI_PASSWORD above) and is the WebSocket *client* - it dials out to the
// laptop's WebSocket *server* at ws://WS_HOST:WS_PORT/ (robot_backend's
// LAPTOP_WS_HOST/LAPTOP_WS_PORT from .env - LAPTOP_WS_HOST must be the
// laptop's real LAN IP for the ESP32 to reach it, not 0.0.0.0). USB is
// power + local debug Serial Monitor output ONLY - no protocol data
// crosses the USB cable anymore.
//
// Every ASCII "line" below is sent as one WebSocket TEXT frame (no '\n'
// needed - WebSocket messages are already framed, unlike the old UART
// byte stream). A raw PCM16LE binary payload, if ever used, is sent as one
// WebSocket BINARY frame.
//
// ESP32 -> laptop (text frames):
//   STATUS:<text>              informational, laptop just logs it
//     STATUS:READY              (re)connected and idle, ready for visitors
//     STATUS:LISTEN_READY       signals the laptop to start recording from
//                                 its own microphone (no audio captured on
//                                 the ESP32)
//     STATUS:GREETING_DONE      greeting animation + chirp complete
//     STATUS:IDLE                interaction finished, back to idle
//   TRIGGER:BUTTON               button pressed while idle
//   TRIGGER:PROXIMITY            someone detected in range while idle
//   ERROR:<text>                 something went wrong on the ESP32 side
//
// laptop -> ESP32 (text frames):
//   COMMAND:GREET                 play wave + greeting chirp (played
//                                  locally over I2S -> MAX98357A if fitted;
//                                  this never involves the laptop)
//   COMMAND:LISTEN                 ESP32 signals LISTEN_READY; laptop
//                                   records from its own mic, then sends
//                                   STATUS:RECORDING_DONE when finished -
//                                   no audio is streamed from the ESP32
//   COMMAND:PROCESSING             status hint only ("Thinking..." on TFT)
//   COMMAND:DISPLAY_A:<text>       status hint only - shows the answer
//                                   text on the TFT
//   COMMAND:SPEAKING               status hint only ("Speaking..." on the
//                                   TFT) while the laptop plays the answer
//                                   out loud on its OWN speakers - no
//                                   audio bytes travel to the board
//   COMMAND:IDLE                   return to idle / reset
//   STATUS:RECORDING_DONE           (a bare line, not COMMAND:-prefixed)
//                                   laptop finished capturing from its
//                                   own mic
//
// Reconnection: on WiFi or WebSocket drop, the ESP32 retries with
// exponential backoff (1s, doubling up to a 30s cap) and prints
// STATUS:RECONNECTING / STATUS:READY to the local USB debug Serial
// Monitor (these can't reach the laptop over the WebSocket while it's
// down). Once reconnected, the ESP32 also sends STATUS:READY over the
// WebSocket, and the laptop resends COMMAND:IDLE to resync state - see
// robot_backend/ws_link.py and main.py.
//
// AUDIO_START:<len>:<crc32> / <len> raw PCM16LE mono 16kHz bytes (as one
// BINARY frame) / AUDIO_END is still defined as a transport primitive in
// robot_backend/ws_link.py (read_audio_frame()/send_audio_frame()) for
// potential future reuse, but nothing calls it today - mic capture and
// TTS playback both happen entirely on the laptop - and this firmware no
// longer contains any code to receive or play such a frame.
//
// Both sides must agree on this exactly - see robot_backend/ws_link.py.
// ============================================================================

#define SERIAL_DEBUG_BAUD 115200

// ============================================================================
// PIN DEFINITIONS (ESP32-S3 DevKit - avoids strapping pins 0/3/45/46 and
// the native-USB pins 19/20)
// ============================================================================
// Triggers
#define PIN_BUTTON   4     // push button, other leg to GND, uses internal pull-up
#define PIN_TRIG     5     // HC-SR04 TRIG
#define PIN_ECHO     6     // HC-SR04 ECHO (use a resistor divider: sensor is 5V logic)

// Actuators / indicators
#define PIN_SERVO    7     // servo signal wire

// 1.8" TFT SPI 128x160 (ST7735)
#define PIN_TFT_RST   8    // RES / RESET
#define PIN_TFT_DC    9    // DC / A0 / RS
#define PIN_TFT_CS   10    // CS
#define PIN_TFT_MOSI 11    // SDA / MOSI / DIN
#define PIN_TFT_SCLK 12    // SCL / SCK / CLK
// Note: Connect TFT BLK/LED to 3.3V, VCC to 3.3V (or 5V), GND to GND

// Microphone: audio is captured by the laptop's own mic.
// The INMP441 I2S mic has been removed. No mic pins are needed on the ESP32.

// MAX98357A amplifier (I2S output, uses I2S peripheral #1) - OPTIONAL,
// only needed if you want the greeting chirp through a real speaker. The
// answer audio is played on the laptop's own speakers, never here.
#define PIN_SPK_BCLK 1     // BCLK
#define PIN_SPK_LRC  2     // LRC / WS
#define PIN_SPK_DOUT 38    // DIN on the MAX98357A. Tie its SD pin high (always on) or to a spare GPIO.

// ============================================================================
// 1.8" TFT SPI CONFIG (ST7735 128x160)
// ============================================================================
#define TFT_WIDTH    160
#define TFT_HEIGHT   128
// Pass the SPI class explicitly to ensure it uses the custom pins on ESP32-S3
Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
bool tftReady = false;

// ============================================================================
// SERVO CONFIG
// ============================================================================
Servo handServo;
#define SERVO_REST_ANGLE 0
#define SERVO_WAVE_ANGLE 90

// ============================================================================
// ULTRASONIC CONFIG
// ============================================================================
#define TRIGGER_DISTANCE_CM   60UL     // start interaction if someone is closer than this
#define ULTRASONIC_TIMEOUT_US 30000UL  // ~5 m max range

// ============================================================================
// AUDIO CONFIG (greeting chirp only - see MAX98357A note above)
// ============================================================================
#define SAMPLE_RATE          16000
#define I2S_SPK_PORT         I2S_NUM_0    // only one I2S port needed (speaker/chirp)
#define CHUNK_SAMPLES         512          // mono samples per I2S write chunk (32ms at 16kHz)
// DMA depth: 4 buffers × 512 stereo samples = 128ms of pipeline depth.
#define I2S_DMA_BUF_COUNT    4
#define I2S_DMA_BUF_LEN      CHUNK_SAMPLES  // stereo samples per DMA buffer
#define WS_CMD_TIMEOUT_MS    20000         // how long to wait for a laptop command

// ============================================================================
// WEBSOCKET CLIENT + RECONNECT STATE
// ============================================================================
WebSocketsClient webSocket;
bool wsConnected = false;

#define WS_BACKOFF_MIN_MS 1000UL
#define WS_BACKOFF_MAX_MS 30000UL
uint32_t wsBackoffMs = WS_BACKOFF_MIN_MS;
unsigned long lastConnectAttempt = 0;

// Small FIFO of incoming TEXT-frame lines, filled by onWsEvent() and drained
// by readLineBlocking() - keeps the rest of the protocol code (waitForCommand,
// streamMicToBackend, etc.) blocking-style and unchanged from the old
// Serial-based version.
#define WS_LINE_QUEUE_LEN 8
String wsLineQueue[WS_LINE_QUEUE_LEN];
uint8_t wsQueueHead = 0, wsQueueTail = 0;

void wsQueuePush(const String &line) {
  uint8_t next = (wsQueueTail + 1) % WS_LINE_QUEUE_LEN;
  if (next == wsQueueHead) {
    // Queue full (shouldn't happen - one command line at a time in this
    // protocol): drop the oldest to make room rather than lose the newest.
    wsQueueHead = (wsQueueHead + 1) % WS_LINE_QUEUE_LEN;
  }
  wsLineQueue[wsQueueTail] = line;
  wsQueueTail = next;
}

bool wsQueuePop(String &out) {
  if (wsQueueHead == wsQueueTail) return false;
  out = wsLineQueue[wsQueueHead];
  wsQueueHead = (wsQueueHead + 1) % WS_LINE_QUEUE_LEN;
  return true;
}

// ============================================================================
// STATE MACHINE
// ============================================================================
enum SystemState {
  STATE_IDLE,
  STATE_GREETING,
  STATE_LISTENING,
  STATE_WAITING_RESPONSE
};
SystemState currentState = STATE_IDLE;

unsigned long lastInteractionTime = 0;
const unsigned long TRIGGER_COOLDOWN_MS = 3000; // ignore new triggers right after one finishes

// Ultrasonic measurement pacing (HC-SR04 requires >= 60ms between pings to prevent echo flooding)
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL_MS = 100;
long lastReportedDistance = -999;

// Forward declaration for display helper
void showStatus(const char *title, const char *body, uint16_t titleColor = 0);
void showIdleDistance(long dist);

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(SERIAL_DEBUG_BAUD);  // USB is power + local debug log only now
  delay(300);

  setupTFT();
  setupServo();
  setupTriggers();
  setupSpeakerI2S();

  showStatus("Starting", "Connecting\nto WiFi...");

  WiFi.mode(WIFI_STA);
  webSocket.onEvent(onWsEvent);
  // We drive our own exponential-backoff reconnect from maintainWiFiAndWs()
  // instead of the library's fixed-interval one, so no setReconnectInterval()
  // call here. The very first WiFi/WebSocket connection also happens lazily
  // from loop() via maintainWiFiAndWs() - nothing to do here but wait.
}

// ============================================================================
// MAIN LOOP
// Idle: pump the WebSocket client, maintain the WiFi/WS connection, and
// (once connected) poll triggers AND watch for an out-of-band COMMAND: line
// from the laptop. Once triggered, run the whole interaction start-to-finish
// (blocking) then return to idle. Only one visitor is served at a time.
// ============================================================================
void loop() {
  webSocket.loop();
  maintainWiFiAndWs();

  if (!wsConnected) return;  // nothing to do until the laptop link is up

  if (currentState == STATE_IDLE) {
    // Drain and ignore any stray laptop command while idle (keeps protocol
    // in sync if the backend restarts mid-session and resends COMMAND:IDLE).
    String stray;
    if (wsQueuePop(stray)) {
      // nothing to act on here; idle already implies COMMAND:IDLE state
    }

    if (millis() - lastInteractionTime < TRIGGER_COOLDOWN_MS) return;

    bool buttonTrigger = digitalRead(PIN_BUTTON) == LOW;      // active-low button

    // Ping ultrasonic sensor with a clean 100ms interval (prevents transducer flooding)
    bool proximityTrigger = false;
    if (millis() - lastPingTime >= PING_INTERVAL_MS) {
      lastPingTime = millis();
      long dist = readDistanceCM();

      // Only refresh screen if distance changes by >= 2cm to keep display smooth
      if (abs(dist - lastReportedDistance) >= 2 || (dist <= TRIGGER_DISTANCE_CM) != (lastReportedDistance <= TRIGGER_DISTANCE_CM)) {
        lastReportedDistance = dist;
        showIdleDistance(dist);
      }

      if (dist > 0 && dist <= TRIGGER_DISTANCE_CM) {
        proximityTrigger = true;
      }
    }

    if (buttonTrigger || proximityTrigger) {
      runInteraction(buttonTrigger ? "BUTTON" : "PROXIMITY");
      lastInteractionTime = millis();
      lastReportedDistance = -999;
    }
  }
}

// ============================================================================
// WIFI / WEBSOCKET CONNECTION MANAGEMENT
// ============================================================================
void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      if (wsConnected) {
        Serial.println("STATUS:RECONNECTING");
      }
      wsConnected = false;
      break;

    case WStype_CONNECTED:
      wsConnected = true;
      wsBackoffMs = WS_BACKOFF_MIN_MS;  // reset backoff after a successful connect
      Serial.println("STATUS:READY");
      sendStatus("READY");
      showStatus("Ready", "Press button\nor stand\nclose to\nstart");
      break;

    case WStype_TEXT:
      // WebSocketsClient null-terminates TEXT frame payloads, so this is
      // safe without using `length` explicitly (Arduino's String has no
      // (buf, length) constructor).
      wsQueuePush(String((char *)payload));
      break;

    case WStype_BIN:
      // Binary frames are a documented transport primitive (see WIRE
      // PROTOCOL) but nothing sends them today. Ignore defensively.
      break;

    default:
      break;
  }
}

// Called every loop() iteration. Owns the exponential-backoff reconnect for
// both WiFi and the WebSocket client, and logs STATUS:RECONNECTING /
// STATUS:READY locally (see the WIRE PROTOCOL note on why these can't
// always reach the laptop).
void maintainWiFiAndWs() {
  if (WiFi.status() != WL_CONNECTED) {
    if (wsConnected) {
      wsConnected = false;
      Serial.println("STATUS:RECONNECTING");
      showStatus("Reconnecting", "WiFi lost...");
    }
    if (millis() - lastConnectAttempt < wsBackoffMs) return;
    lastConnectAttempt = millis();
    Serial.printf("Connecting to WiFi SSID '%s' (next retry in %lums if this fails)...\n",
                  WIFI_SSID, (unsigned long)wsBackoffMs);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wsBackoffMs = min(wsBackoffMs * 2, WS_BACKOFF_MAX_MS);
    return;
  }

  if (!webSocket.isConnected()) {
    if (millis() - lastConnectAttempt < wsBackoffMs) return;
    lastConnectAttempt = millis();
    Serial.printf("Connecting WebSocket to %s:%d (next retry in %lums if this fails)...\n",
                  WS_HOST, WS_PORT, (unsigned long)wsBackoffMs);
    showStatus("Reconnecting", "to laptop...");
    webSocket.begin(WS_HOST, WS_PORT, WS_PATH);
    wsBackoffMs = min(wsBackoffMs * 2, WS_BACKOFF_MAX_MS);
  }
}

// ============================================================================
// FULL INTERACTION SEQUENCE
// ============================================================================
void runInteraction(const char *triggerSource) {
  // WiFi/WebSocket must stay up throughout - it's the only link to the
  // laptop now (previously it was toggled off here purely to save power,
  // since USB serial carried the protocol; that trick no longer applies).
  Serial.print("TRIGGER:");
  Serial.println(triggerSource);
  wsSendLine(String("TRIGGER:") + triggerSource);

  // --- 1. Wait for COMMAND:GREET from the backend ---
  String cmd = waitForCommand(WS_CMD_TIMEOUT_MS);
  if (cmd != "GREET") {
    sendError("Expected GREET, got: " + cmd);
    returnToIdle();
    return;
  }
  currentState = STATE_GREETING;
  showStatus("Hello!", "Ask your\nquestion\nafter beep");
  waveServo();
  playGreetingTone();
  drainAudioPipeline(); // flush DMA before any subsequent TFT SPI access
  sendStatus("GREETING_DONE");

  // --- 2. Wait for COMMAND:LISTEN, then let the laptop record+confirm ---
  cmd = waitForCommand(WS_CMD_TIMEOUT_MS);
  if (cmd != "LISTEN") {
    sendError("Expected LISTEN, got: " + cmd);
    returnToIdle();
    return;
  }
  currentState = STATE_LISTENING;
  showStatus("Listening...", "Speak your\nquestion\nnow");
  streamMicToBackend();

  // --- 3. Optional COMMAND:PROCESSING / DISPLAY_A / SPEAKING status hints,
  //        then wait for the final IDLE ---
  currentState = STATE_WAITING_RESPONSE;
  showStatus("Thinking...", "Please wait");

  // Save the displayed text so we can re-show it on the TFT after the
  // laptop finishes playing the answer on its own speakers. Without this,
  // returnToIdle() immediately overwrites the answer with "Ready" the
  // moment IDLE arrives, making the TFT appear de-synced from the Python
  // terminal which still shows the answer.
  String lastAnswer = "";

  cmd = waitForCommand(WS_CMD_TIMEOUT_MS);
  if (cmd == "PROCESSING") {
    cmd = waitForCommand(WS_CMD_TIMEOUT_MS);
  }

  if (cmd.startsWith("DISPLAY_Q:")) {
    // Question is displayed in the laptop terminal only, not on the TFT
    cmd = waitForCommand(WS_CMD_TIMEOUT_MS);
  }

  if (cmd.startsWith("DISPLAY_A:")) {
    lastAnswer = cmd.substring(10);
    showStatus("Answer:", lastAnswer.c_str(), ST7735_CYAN);
    cmd = waitForCommand(WS_CMD_TIMEOUT_MS);
  }

  if (cmd == "SPEAKING") {
    // Status hint only (same idea as PROCESSING) - the laptop is playing
    // the answer on its own speakers right now; no audio bytes travel to
    // the board. We just wait for the IDLE that follows once it's done.
    showStatus("Speaking...", "Answer is\nplaying on\nthe laptop", ST7735_CYAN);
    cmd = waitForCommand(WS_CMD_TIMEOUT_MS);
  }

  if (cmd == "IDLE") {
    if (lastAnswer.length() > 0) {
      showStatus("Answer:", lastAnswer.c_str(), ST7735_CYAN);
      delay(4000); // keep the answer visible for 4 seconds
    }
  } else {
    sendError("Expected SPEAKING or IDLE, got: " + cmd);
  }

  returnToIdle();
}

void returnToIdle() {
  currentState = STATE_IDLE;
  handServo.write(SERVO_REST_ANGLE);
  showStatus("Ready", "Press button\nor stand\nclose to\nstart");
  lastReportedDistance = -999;
  sendStatus("IDLE");
}

// ============================================================================
// WEBSOCKET LINE HELPERS
// ============================================================================
void wsSendLine(const String &text) {
  if (webSocket.isConnected()) {
    webSocket.sendTXT(text);
  }
}

void sendStatus(const String &text) {
  wsSendLine("STATUS:" + text);
}

void sendError(const String &text) {
  wsSendLine("ERROR:" + text);
}

// Blocks until a "COMMAND:<word>" line arrives (or timeout), returns <word>.
// Returns "" on timeout. Any non-COMMAND line received while waiting is
// ignored (defensive against stray STATUS echoes etc.).
String waitForCommand(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (true) {
    unsigned long elapsed = millis() - start;
    if (elapsed >= timeoutMs) return "";
    // Poll in 100ms slices so we check the overall deadline frequently.
    unsigned long slice = timeoutMs - elapsed;
    if (slice > 100) slice = 100;
    String line = readLineBlocking(slice);
    if (line.length() == 0) continue;
    if (line.startsWith("COMMAND:")) {
      return line.substring(8);
    }
    // Non-COMMAND lines (STATUS echoes, stray bytes) are discarded; keep waiting.
  }
}

// ============================================================================
// TRIGGER HARDWARE: button + HC-SR04
// ============================================================================
void setupTriggers() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);
}

// Returns distance in cm, or a large number if out of range/timeout.
long readDistanceCM() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long duration = pulseIn(PIN_ECHO, HIGH, ULTRASONIC_TIMEOUT_US);
  if (duration == 0) return 9999; // no echo received = nothing in range

  return (long)(duration * 0.0343 / 2.0); // speed of sound conversion
}

// ============================================================================
// SERVO
// ============================================================================
void setupServo() {
  ESP32PWM::allocateTimer(0);
  handServo.setPeriodHertz(50);
  handServo.attach(PIN_SERVO, 500, 2400);
  handServo.write(SERVO_REST_ANGLE);
}

void waveServo() {
  for (int i = 0; i < 3; i++) {
    handServo.write(SERVO_WAVE_ANGLE);
    delay(300);
    handServo.write(SERVO_REST_ANGLE);
    delay(300);
  }
}

// ============================================================================
// TFT DISPLAY - pure status feedback, no laptop involvement
// ============================================================================
void setupTFT() {
  // Pass MISO=-1 (not used), MOSI and SCLK as defined, CS managed by Adafruit.
  // 27 MHz is safe for ST7735 on short PCB traces; reduces per-frame latency.
  SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  SPI.setFrequency(27000000);
  // Initialization for 1.8" TFT SPI 128x160 (ST7735)
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // landscape mode: 160 width x 128 height
  tft.fillScreen(ST7735_BLACK);
  tftReady = true;
}

void showStatus(const char *title, const char *body, uint16_t titleColor) {
  if (!tftReady) return;

  if (titleColor == 0) {
    if (strcmp(title, "Ready") == 0) titleColor = ST7735_GREEN;
    else if (strcmp(title, "Hello!") == 0) titleColor = ST7735_CYAN;
    else if (strcmp(title, "Listening...") == 0) titleColor = ST7735_GREEN;
    else if (strcmp(title, "Thinking...") == 0) titleColor = ST7735_YELLOW;
    else if (strcmp(title, "Speaking...") == 0) titleColor = ST7735_CYAN;
    else if (strcmp(title, "Answering...") == 0) titleColor = ST7735_CYAN;
    else titleColor = ST7735_WHITE;
  }

  tft.fillScreen(ST7735_BLACK);

  // Top header banner
  tft.fillRect(0, 0, 160, 18, ST7735_BLUE);
  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(44, 5);
  tft.print("EVENT ROBOT");

  // Title in thematic color
  tft.setTextSize(1);
  tft.setTextColor(titleColor);
  tft.setCursor(5, 22);
  tft.println(title);

  // Divider line
  tft.drawFastHLine(5, 33, 150, 0x4208); // subtle gray divider line

  // Body message
  //
  // Choose text size based on the LONGEST pipe-separated segment, not the
  // total body length.  At textSize=2 each character is 12px wide; with a
  // 5px left margin on a 160px-wide display only 12 characters fit before
  // GFX auto-wraps the line internally.
  // The Python side word-wraps at 24 chars, so multi-sentence answers
  // are 13-24 chars and use textSize=1 (6px/char, ~25 chars fit).
  // Status screens ("Ready", "Hello!", "Listening...", "Thinking...")
  // have segments <= 12 chars and use textSize=2 (big, clear font).
  int maxSegLen = 0, segLen = 0;
  for (const char *pp = body; *pp; pp++) {
    if (*pp == '|' || *pp == '\n') {
      if (segLen > maxSegLen) maxSegLen = segLen;
      segLen = 0;
    } else {
      segLen++;
    }
  }
  if (segLen > maxSegLen) maxSegLen = segLen;

  int textSize    = (maxSegLen > 12) ? 1 : 2;
  int lineSpacing = (textSize == 1) ? 10 : 16;

  tft.setTextSize(textSize);
  tft.setTextColor(ST7735_WHITE);

  // Body starts at y=34.
  // At textSize=2 with lineSpacing=16, 5 lines occupy y=34, 50, 66, 82, 98.
  // The 5th line ends at y=111 <= 127 (fits completely within TFT height).
  // For Ready screen: 4 lines end at y=95, well above the sensor area (y=107..127).
  int cursorY = 34;
  tft.setCursor(5, cursorY);

  int maxCursorY = (textSize == 2) ? 102 : 118;

  const char *p = body;
  while (*p) {
    if (*p == '|' || *p == '\n') {
      cursorY += lineSpacing;
      if (cursorY > maxCursorY) break; // clamp: don't overflow display or sensor zone
      tft.setCursor(5, cursorY);
    } else {
      tft.print(*p);
    }
    p++;
  }
}

void showIdleDistance(long dist) {
  if (!tftReady) return;
  // Clear only the bottom sensor status area (y=107 to 127) to avoid screen flicker
  // and preserve all 4 lines of textSize=2 body text above (ends at y=102).
  tft.fillRect(0, 107, 160, 21, ST7735_BLACK);
  tft.setCursor(5, 114);
  tft.setTextSize(1);

  if (dist >= 9999) {
    tft.setTextColor(ST7735_RED);
    tft.print("Sensor: No echo/out");
  } else if (dist <= TRIGGER_DISTANCE_CM) {
    tft.setTextColor(ST7735_GREEN);
    tft.print("Sensor: NEAR (");
    tft.print(dist);
    tft.print("cm)");
  } else {
    tft.setTextColor(ST7735_CYAN);
    tft.print("Sensor: ");
    tft.print(dist);
    tft.print(" cm");
  }
}

// ============================================================================
// MICROPHONE - laptop mic (no local I2S mic hardware)
// ============================================================================
// Audio capture is entirely on the laptop. When the backend sends
// COMMAND:LISTEN, the ESP32 signals readiness with STATUS:LISTEN_READY, then
// blocks waiting for STATUS:RECORDING_DONE from the laptop (sent once the
// laptop has finished recording from its own microphone and is about to run
// STT). No audio data is transmitted from the ESP32 to the laptop at all.
void streamMicToBackend() {
  // Signal the laptop to start capturing from its own microphone.
  sendStatus("LISTEN_READY");

  // Wait until the laptop confirms it has finished recording.
  // The backend sends back "STATUS:RECORDING_DONE" as a plain line
  // (not a COMMAND: prefix) to distinguish it from the normal command flow.
  unsigned long start = millis();
  while (millis() - start < WS_CMD_TIMEOUT_MS) {
    String line = readLineBlocking(200);
    if (line == "STATUS:RECORDING_DONE") return;
    // Ignore stray lines (STATUS echoes, etc.) and keep waiting.
  }
  sendError("Timed out waiting for STATUS:RECORDING_DONE from laptop mic");
}

// ============================================================================
// SPEAKER (MAX98357A, optional) - I2S output for the LOCAL greeting chirp
// only. Nothing is ever received from the laptop and played here anymore -
// see the WIRE PROTOCOL note above.
// ============================================================================
void setupSpeakerI2S() {
  i2s_config_t spkConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // MAX98357A wants a stereo frame
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    // I2S_DMA_BUF_LEN is in stereo samples (CHUNK_SAMPLES*2).
    // 4 buffers at that size gives ~128ms of audio pipeline depth,
    // eliminating DMA underruns that cause crackling.
    .dma_buf_count = I2S_DMA_BUF_COUNT,
    .dma_buf_len = I2S_DMA_BUF_LEN,
    .use_apll = true,   // use APLL for a cleaner clock - reduces jitter/noise
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t spkPins = {
    .bck_io_num = PIN_SPK_BCLK,
    .ws_io_num = PIN_SPK_LRC,
    .data_out_num = PIN_SPK_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_SPK_PORT, &spkConfig, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &spkPins);
  i2s_zero_dma_buffer(I2S_SPK_PORT);
}

// Software volume scale: 0.0 (silent) to 1.0 (full).
#define AUDIO_VOLUME_SCALE  0.70f

// Writes one chunk of mono 16-bit PCM samples out to the speaker,
// duplicating each sample into a stereo (L=R) frame.
void playMonoPCM(const int16_t *mono, size_t sampleCount) {
  static int16_t stereo[CHUNK_SAMPLES * 2];
  size_t offset = 0;
  while (offset < sampleCount) {
    size_t n = sampleCount - offset;
    if (n > CHUNK_SAMPLES) n = CHUNK_SAMPLES;
    for (size_t i = 0; i < n; i++) {
      int32_t s = (int32_t)(mono[offset + i] * AUDIO_VOLUME_SCALE);
      // Clamp to int16 range to prevent wrap-around distortion
      if (s >  32767) s =  32767;
      if (s < -32768) s = -32768;
      stereo[2 * i]     = (int16_t)s;
      stereo[2 * i + 1] = (int16_t)s;
    }
    size_t bytesWritten = 0;
    i2s_write(I2S_SPK_PORT, stereo, n * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    offset += n;
  }
}

// Writes one buffer of silence then delays long enough for all DMA buffers
// to drain at the hardware level.  Call after every local audio sequence
// (the greeting chirp) so the I2S GDMA is fully idle before returnToIdle()
// writes to the TFT - otherwise the SPI GDMA and I2S GDMA clash, producing
// the simultaneous crackle + display glitch observed during testing.
void drainAudioPipeline() {
  // One extra chunk of silence pushes any partial last DMA buffer through.
  static int16_t silence[CHUNK_SAMPLES] = {0};
  playMonoPCM(silence, CHUNK_SAMPLES);
  // Wait for all remaining DMA buffers to finish playing.
  // drain_ms = (buf_count * buf_len samples) / sample_rate
  const uint32_t drainMs =
      ((uint32_t)I2S_DMA_BUF_COUNT * I2S_DMA_BUF_LEN * 1000UL) / SAMPLE_RATE;
  delay(drainMs + 50); // +50 ms safety margin
}

// A short two-tone chirp played locally (no laptop round trip) right after a
// trigger, so the visitor gets an instant audible cue to start speaking.
void playGreetingTone() {
  static int16_t tone[CHUNK_SAMPLES];
  const float freqs[2] = {880.0f, 1320.0f};

  for (int t = 0; t < 2; t++) {
    uint32_t phaseCounter = 0;
    for (int rep = 0; rep < (SAMPLE_RATE / 4) / CHUNK_SAMPLES; rep++) {
      for (int i = 0; i < CHUNK_SAMPLES; i++) {
        float angle = 2.0f * PI * freqs[t] * ((float)phaseCounter / SAMPLE_RATE);
        tone[i] = (int16_t)(3000.0f * sinf(angle));
        phaseCounter++;
      }
      playMonoPCM(tone, CHUNK_SAMPLES);
    }
  }
}

// Blocks until a full line is available from the incoming WebSocket TEXT
// frame queue, or timeout. Pumps webSocket.loop() on every poll so this
// works correctly even when called from deep inside a long blocking wait
// (waitForCommand, streamMicToBackend) that doesn't return to the top-level
// loop() until a whole interaction ends - WebSocketsClient needs .loop()
// called frequently to turn incoming TCP data into queued TEXT frames.
String readLineBlocking(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    webSocket.loop();
    String line;
    if (wsQueuePop(line)) {
      line.trim();
      return line;
    }
    delay(2);
  }
  return "";
}
