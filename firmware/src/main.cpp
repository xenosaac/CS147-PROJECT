// main.cpp — Construction Site Worker Safety Monitor, ESP32 firmware (LILYGO T-Display); hand-built JSON POST body (no ArduinoJson).

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT20.h>

#include "config.h"
#if __has_include("build_info.h")
  #include "build_info.h"
#endif

// ENABLE_DISPLAY must be fully isolating: everything else in this file must
// compile and run identically whether it's 0 or 1. TFT_eSPI is only ever
// included/instantiated inside these guards.
#if ENABLE_DISPLAY
  #include <TFT_eSPI.h>
#endif

// GPIO pin table — matches physical rig (updated 2026-07-31); SDA/SCL swapped, buzzer/LED moved since GPIO25 is now the CAP1188's SPI clock line.
#define PIN_DHT20_SDA   21
#define PIN_DHT20_SCL   22
#define PIN_BUZZER      17
#define PIN_LED         2
#define PIN_LED_YELLOW  12
#define PIN_LED_BLUE    13
static const uint8_t DHT20_I2C_ADDRESS = 0x38;
// RIGHT_BUTTON (GPIO35) is board-defined (pins_arduino.h), not redefined here.
// Input-only, no internal pull-up; board supplies bias, LOW = pressed.
// GPIO34 (VBAT) is hardwired to the battery divider — never repurpose it.

// ---------------------------------------------------------------------------
// Firmware behavior constants
// ---------------------------------------------------------------------------
static const unsigned long SAMPLE_INTERVAL_MS        = 3000;   // DHT20 read cadence; DHT20 needs >=1s between reads.
static const unsigned long SENSOR_RETRY_INTERVAL_MS  = 10000;  // Bound I2C retries after a bus/sensor fault.
static const uint8_t       MAX_CONSECUTIVE_DHT_DATA_ERRORS = 3;
static const float         HIGH_HEAT_THRESHOLD_C      = 35.0f; // 95F, Cal/OSHA Title 8 Section 3395 "high-heat" trigger.

// PWM (not digitalWrite) so drive voltage/loudness is tunable without rewiring.
// 20kHz carrier is above audible range, so it acts as a duty-cycle divider only.
static const uint32_t      BUZZER_PWM_FREQ_HZ         = 20000;
static const uint8_t       BUZZER_PWM_RESOLUTION_BITS = 8;
static const uint8_t       BUZZER_PWM_DUTY_ON         = 60;   // ~24% of full volume; tune to taste.

// DEMO_FAST_TIMING compresses timer transitions (not the heat threshold) so a
// demo video can show the full state sequence in one take. Never flash =1
// outside filming.
#ifndef DEMO_FAST_TIMING
#define DEMO_FAST_TIMING 0
#endif

#if DEMO_FAST_TIMING
static const unsigned long SUSTAINED_HIGH_HEAT_SEC      = 10;  // DEMO override, real value 600 (10 min).
static const unsigned long REMINDER_INTERVAL_NORMAL_SEC = 20;  // DEMO override, real value 1800 (30 min).
static const unsigned long REMINDER_INTERVAL_HOT_SEC     = 10;  // DEMO override, real value 900 (15 min).
static const unsigned long CHECKIN_GRACE_SEC             = 15;  // DEMO override, real value 300 (5 min).
#else
static const unsigned long SUSTAINED_HIGH_HEAT_SEC      = 600;   // 10 min continuous above threshold before interval shortens.
static const unsigned long REMINDER_INTERVAL_NORMAL_SEC = 1800;  // 30 min base water-reminder interval.
static const unsigned long REMINDER_INTERVAL_HOT_SEC     = 900;  // 15 min shortened interval once sustained high heat hits.
static const unsigned long CHECKIN_GRACE_SEC             = 300;  // 5 min after a prompt fires before escalation.
#endif

// ---------------------------------------------------------------------------
// Alert level state machine
// ---------------------------------------------------------------------------
enum AlertLevel {
  ALERT_NORMAL = 0,
  ALERT_HIGH_HEAT,
  ALERT_REMINDER_DUE,
  ALERT_ESCALATED
};

// Must match "normal" | "high_heat" | "reminder_due" | "escalated" verbatim
// (required by the backend's HTTP API contract).
static const char *alertLevelToString(AlertLevel level) {
  switch (level) {
    case ALERT_NORMAL:       return "normal";
    case ALERT_HIGH_HEAT:    return "high_heat";
    case ALERT_REMINDER_DUE: return "reminder_due";
    case ALERT_ESCALATED:    return "escalated";
  }
  return "normal";
}

// ---------------------------------------------------------------------------
// Global sensor / state variables
// ---------------------------------------------------------------------------
DHT20 dht20(&Wire);

float g_tempC       = 0.0f;
float g_humidityPct = 0.0f;
float g_heatIndexC  = 0.0f;
bool  g_haveReading  = false; // true once at least one successful DHT20 read has happened
bool  g_wireReady    = false;
bool  g_wireOwned    = false; // true from successful Wire.begin until Wire.end confirms teardown
bool  g_sensorReady  = false;
bool  g_deploymentConfigReady = false;
uint8_t g_consecutiveDhtDataErrors = 0;

AlertLevel g_alertLevel = ALERT_NORMAL;

unsigned long g_highHeatStartMs = 0; // millis() timestamp heat index first crossed threshold, 0 = not currently high
unsigned long g_lastCheckinMs   = 0; // millis() timestamp of last check-in (boot counts as one)
unsigned long g_reminderFiredMs = 0; // millis() timestamp the current reminder_due prompt fired, 0 = none pending
unsigned long g_reminderIntervalSec = REMINDER_INTERVAL_NORMAL_SEC; // currently active interval

// Edge-triggered flag: true only for the single POST immediately following a
// button press, per the HTTP contract's "checkinPressed" field.
bool g_checkinPressedEdge = false;

unsigned long g_lastSampleMs = 0;
unsigned long g_lastSensorInitAttemptMs = 0;

// Heat index (NOAA/Rothfusz regression); computed in F, converted back to C.
static float computeHeatIndexF(float tempF, float rh) {
  if (tempF < 80.0f) {
    // NOAA simple linear approximation.
    return 0.5f * (tempF + 61.0f + ((tempF - 68.0f) * 1.2f) + (rh * 0.094f));
  }

  // Full NOAA/Rothfusz regression, valid (with best accuracy) for tempF >= 80F.
  float T = tempF;
  float R = rh;
  float hi = -42.379f
           + 2.04901523f   * T
           + 10.14333127f  * R
           - 0.22475541f   * T * R
           - 0.00683783f   * T * T
           - 0.05481717f   * R * R
           + 0.00122874f   * T * T * R
           + 0.00085282f   * T * R * R
           - 0.00000199f   * T * T * R * R;
  return hi;
}

static float computeHeatIndexC(float tempC, float rh) {
  float tempF = (tempC * 9.0f / 5.0f) + 32.0f;
  float hiF = computeHeatIndexF(tempF, rh);
  return (hiF - 32.0f) * 5.0f / 9.0f;
}

// Display module — isolated behind ENABLE_DISPLAY; no-op stubs when 0.
#if ENABLE_DISPLAY

TFT_eSPI tft = TFT_eSPI();

// Off-screen composition buffer: drawing here then pushSprite()-ing in one
// shot avoids the visible black-flash that rate-limiting alone didn't fix.
TFT_eSprite frameBuffer = TFT_eSprite(&tft);

static uint16_t colorForAlertLevel(AlertLevel level) {
  switch (level) {
    case ALERT_NORMAL:       return TFT_GREEN;
    case ALERT_HIGH_HEAT:    return TFT_ORANGE;
    case ALERT_REMINDER_DUE: return TFT_YELLOW;
    case ALERT_ESCALATED:    return TFT_RED;
  }
  return TFT_WHITE;
}

static inline float cToF(float celsius) {
  return celsius * 9.0f / 5.0f + 32.0f;
}

static void displayInit() {
  tft.init();
  tft.setRotation(1); // landscape, 240x135
  tft.fillScreen(TFT_BLACK);

  // 8-bit color depth keeps the sprite to ~32KB (240*135*1) instead of ~65KB
  // at 16-bit — plenty of palette range for this UI's handful of named
  // TFT_eSPI colors, and lighter on ESP32 heap.
  frameBuffer.setColorDepth(8);
  frameBuffer.createSprite(tft.width(), tft.height());
  frameBuffer.setTextColor(TFT_WHITE, TFT_BLACK);
  frameBuffer.setTextSize(1);
  frameBuffer.fillSprite(TFT_BLACK);
  frameBuffer.setCursor(0, 0, 2);
  frameBuffer.println(" Worker Safety Monitor");
  frameBuffer.println(" Booting...");
  frameBuffer.pushSprite(0, 0);
}

// Throttled to ~1 Hz since data only changes every SAMPLE_INTERVAL_MS; the
// flicker fix itself is the sprite buffering above, not this rate limit.
static const unsigned long DISPLAY_REFRESH_INTERVAL_MS = 1000;

static void displayUpdate(bool wifiConnected, unsigned long secondsSinceCheckin) {
  static unsigned long lastDrawMs = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastDrawMs < DISPLAY_REFRESH_INTERVAL_MS) {
    return;
  }
  lastDrawMs = nowMs;

  frameBuffer.fillSprite(TFT_BLACK); // clears the off-screen buffer only — not visible

  // Top status bar, colored by alert level.
  uint16_t bandColor = colorForAlertLevel(g_alertLevel);
  frameBuffer.fillRect(0, 0, frameBuffer.width(), 22, bandColor);
  frameBuffer.setTextColor(TFT_BLACK, bandColor);
  frameBuffer.setCursor(4, 4, 2);
  frameBuffer.print(alertLevelToString(g_alertLevel));

  frameBuffer.setTextColor(TFT_WHITE, TFT_BLACK);
  frameBuffer.setCursor(4, 28, 2);
  frameBuffer.printf("Temp:  %.1f F", cToF(g_tempC));

  frameBuffer.setCursor(4, 46, 2);
  frameBuffer.printf("Humid: %.1f %%", g_humidityPct);

  frameBuffer.setCursor(4, 64, 2);
  frameBuffer.printf("Heat idx: %.1f F", cToF(g_heatIndexC));

  frameBuffer.setCursor(4, 82, 2);
  frameBuffer.printf("Interval: %lu s", g_reminderIntervalSec);

  frameBuffer.setCursor(4, 100, 2);
  frameBuffer.printf("Since check-in: %lus", secondsSinceCheckin);

  frameBuffer.setCursor(4, 118, 2);
  frameBuffer.printf("WiFi: %s", wifiConnected ? "connected" : "offline");

  if (g_alertLevel == ALERT_ESCALATED) {
    // Escalated screen shows "UNCONFIRMED".
    frameBuffer.setTextColor(TFT_RED, TFT_BLACK);
    frameBuffer.setCursor(120, 4, 2);
    frameBuffer.print("UNCONFIRMED");
  }

  // Single atomic blit — the only thing the physical panel ever shows.
  frameBuffer.pushSprite(0, 0);
}

#else // ENABLE_DISPLAY == 0 : no-op stubs, zero TFT_eSPI footprint.

static inline void displayInit() {}
static inline void displayUpdate(bool /*wifiConnected*/, unsigned long /*secondsSinceCheckin*/) {}

#endif // ENABLE_DISPLAY

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static bool deploymentConfigIsReady() {
  const bool wifiReady = strlen(WIFI_SSID) > 0 &&
                         strcmp(WIFI_SSID, "YOUR_WIFI_SSID_HERE") != 0 &&
                         strlen(WIFI_PASSWORD) > 0 &&
                         strcmp(WIFI_PASSWORD, "YOUR_WIFI_PASSWORD_HERE") != 0;
  const bool serverReady = strlen(SERVER_HOST) > 0 &&
                           strcmp(SERVER_HOST, "YOUR_SERVER_HOST_OR_IP_HERE") != 0;
  return wifiReady && serverReady;
}

static void connectWiFi() {
  Serial.printf("Connecting to WiFi SSID '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    // Non-fatal: safety logic must keep working with no network; retried
    // in the background from loop().
    Serial.println("WiFi connect timed out; will keep retrying in background.");
  }
}

// Called periodically from loop() so a dropped connection is retried without
// ever blocking the sampling/alert loop.
static void maintainWiFi() {
  static unsigned long lastAttemptMs = 0;
  const unsigned long RETRY_INTERVAL_MS = 10000;

  if (!g_deploymentConfigReady || WiFi.status() == WL_CONNECTED) {
    return;
  }
  unsigned long now = millis();
  if (now - lastAttemptMs >= RETRY_INTERVAL_MS) {
    lastAttemptMs = now;
    Serial.println("WiFi not connected; retrying...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// Button (RIGHT_BUTTON, GPIO35, board-defined) — debounced edge detection, LOW = pressed.
static bool pollButtonPressedEdge() {
  static int lastReading = HIGH;
  static int stableState = HIGH;
  static unsigned long lastChangeMs = 0;
  const unsigned long DEBOUNCE_MS = 40;

  int reading = digitalRead(RIGHT_BUTTON);
  if (reading != lastReading) {
    lastChangeMs = millis();
    lastReading = reading;
  }

  bool pressedEdge = false;
  if ((millis() - lastChangeMs) > DEBOUNCE_MS && reading != stableState) {
    stableState = reading;
    if (stableState == LOW) {
      pressedEdge = true; // LOW = pressed
    }
  }
  return pressedEdge;
}

// Any button press, at any time, resets the reminder timer and clears
// reminder_due/escalated back toward normal/high_heat.
static void onButtonPress(unsigned long nowMs) {
  Serial.println("Check-in button pressed.");
  g_lastCheckinMs = nowMs;
  g_reminderFiredMs = 0;
  g_checkinPressedEdge = true;
  g_alertLevel = (g_heatIndexC >= HIGH_HEAT_THRESHOLD_C) ? ALERT_HIGH_HEAT : ALERT_NORMAL;
}

// ---------------------------------------------------------------------------
// DHT20 sampling
// ---------------------------------------------------------------------------
enum SensorSampleResult {
  SENSOR_SAMPLE_OK,
  SENSOR_SAMPLE_DEFERRED,
  SENSOR_SAMPLE_FAULT
};

static bool i2cLinesReleased() {
  return digitalRead(PIN_DHT20_SDA) == HIGH &&
         digitalRead(PIN_DHT20_SCL) == HIGH;
}

static bool releaseI2CBus() {
  // No clock-based bus clear against an unknown external clamp — just end the
  // peripheral and go passive until pull-ups prove the bus is free again.
  g_wireReady = false;
  if (g_wireOwned && !Wire.end()) {
    Serial.println("[FAIL] Wire.end did not release the I2C peripheral; retry deferred.");
    return false;
  }
  g_wireOwned = false;
  pinMode(PIN_DHT20_SDA, INPUT);
  pinMode(PIN_DHT20_SCL, INPUT);
  return true;
}

static bool initializeI2CBus() {
  // If an earlier teardown failed, do not mix GPIO ownership with a still-live
  // I2C peripheral. Retry the passive teardown first at the existing cadence.
  if (g_wireOwned && !releaseI2CBus()) {
    return false;
  }
  pinMode(PIN_DHT20_SDA, INPUT);
  pinMode(PIN_DHT20_SCL, INPUT);
  delayMicroseconds(100);
  if (!i2cLinesReleased()) {
    g_wireReady = false;
    Serial.println("[SKIP] Wire init: SDA or SCL is externally held LOW.");
    return false;
  }

  bool began = Wire.begin(PIN_DHT20_SDA, PIN_DHT20_SCL);
  g_wireOwned = began;
  bool clockReady = began && Wire.setClock(100000);
  if (clockReady) {
    Wire.setTimeOut(20);
  } else if (began) {
    releaseI2CBus();
  }
  g_wireReady = began && clockReady;
  Serial.printf("[%s] ESP32 Wire.begin + 100 kHz clock\n",
                g_wireReady ? "PASS" : "FAIL");
  return g_wireReady;
}

static bool dht20AddressResponds() {
  Wire.beginTransmission(DHT20_I2C_ADDRESS);
  uint8_t error = Wire.endTransmission(true);
  if (error != 0 || !i2cLinesReleased()) {
    Serial.printf("DHT20 address preflight failed: Wire error=%u.\n", error);
    return false;
  }
  return true;
}

static SensorSampleResult sampleSensor() {
  // DHT20 0.3.2 can poll ~1s after certain Wire errors; the passive line
  // check below avoids that when the bus is already clamped.
  if (!g_wireReady || !i2cLinesReleased()) {
    Serial.println("DHT20 read skipped: SDA or SCL is held LOW.");
    g_haveReading = false;
    return SENSOR_SAMPLE_FAULT;
  }
  if (!dht20AddressResponds()) {
    g_haveReading = false;
    return SENSOR_SAMPLE_FAULT;
  }

  int status = dht20.read(); // blocking synchronous read + convert
  if (!i2cLinesReleased()) {
    Serial.println("DHT20 read fault: SDA or SCL became LOW during the transaction.");
    g_consecutiveDhtDataErrors = 0;
    g_haveReading = false;
    return SENSOR_SAMPLE_FAULT;
  }
  if (status == DHT20_ERROR_LASTREAD) {
    // Cadence signal, not a fault — library needs 1s from boot before first read.
    Serial.println("DHT20 sample deferred: minimum 1 s interval has not elapsed.");
    return SENSOR_SAMPLE_DEFERRED;
  }
  if (status == DHT20_ERROR_CHECKSUM || status == DHT20_ERROR_BYTES_ALL_ZERO) {
    // One bad payload isn't proof Wire is wedged, but persistent corruption
    // must not silently disable telemetry forever.
    if (g_consecutiveDhtDataErrors < MAX_CONSECUTIVE_DHT_DATA_ERRORS) {
      ++g_consecutiveDhtDataErrors;
    }
    g_haveReading = false;
    if (g_consecutiveDhtDataErrors >= MAX_CONSECUTIVE_DHT_DATA_ERRORS) {
      Serial.printf("DHT20 persistent data error: %d (%u/%u); rebuilding Wire.\n",
                    status, g_consecutiveDhtDataErrors,
                    MAX_CONSECUTIVE_DHT_DATA_ERRORS);
      return SENSOR_SAMPLE_FAULT;
    }
    Serial.printf("DHT20 transient data error: %d (%u/%u); Wire remains active.\n",
                  status, g_consecutiveDhtDataErrors,
                  MAX_CONSECUTIVE_DHT_DATA_ERRORS);
    return SENSOR_SAMPLE_DEFERRED;
  }
  if (status != DHT20_OK) {
    Serial.printf("DHT20 read error: %d\n", status);
    g_consecutiveDhtDataErrors = 0;
    g_haveReading = false;
    return SENSOR_SAMPLE_FAULT;
  }
  g_consecutiveDhtDataErrors = 0;
  g_tempC = dht20.getTemperature();
  g_humidityPct = dht20.getHumidity();
  g_heatIndexC = computeHeatIndexC(g_tempC, g_humidityPct);
  g_haveReading = true;
  return SENSOR_SAMPLE_OK;
}

static bool initializeSensor() {
  g_lastSensorInitAttemptMs = millis();
  if (!i2cLinesReleased()) {
    Serial.println("DHT20 init skipped: SDA or SCL is held LOW.");
    g_sensorReady = false;
    return false;
  }
  if (!g_wireReady && !initializeI2CBus()) {
    Serial.println("DHT20 init skipped: ESP32 I2C peripheral is unavailable.");
    g_sensorReady = false;
    return false;
  }

  g_sensorReady = dht20.begin();
  Serial.printf("[%s] DHT20 I2C preflight\n", g_sensorReady ? "PASS" : "FAIL");
  if (g_sensorReady) {
    g_consecutiveDhtDataErrors = 0;
  } else {
    releaseI2CBus();
  }
  return g_sensorReady;
}

// Alert-level state machine — device-local, must keep working with no WiFi/cloud.
static void updateAlertState(unsigned long nowMs) {
  // Track continuous time above the high-heat threshold.
  if (g_heatIndexC >= HIGH_HEAT_THRESHOLD_C) {
    if (g_highHeatStartMs == 0) {
      g_highHeatStartMs = nowMs;
    }
  } else {
    g_highHeatStartMs = 0;
  }

  // Pick the currently active reminder interval based on sustained heat.
  if (g_highHeatStartMs != 0 &&
      (nowMs - g_highHeatStartMs) / 1000UL >= SUSTAINED_HIGH_HEAT_SEC) {
    g_reminderIntervalSec = REMINDER_INTERVAL_HOT_SEC;
  } else {
    g_reminderIntervalSec = REMINDER_INTERVAL_NORMAL_SEC;
  }

  unsigned long secondsSinceCheckin = (nowMs - g_lastCheckinMs) / 1000UL;

  switch (g_alertLevel) {
    case ALERT_ESCALATED:
      // Persistent alarm; cleared only by a button press (onButtonPress()).
      break;

    case ALERT_REMINDER_DUE: {
      unsigned long secondsSinceFired = (nowMs - g_reminderFiredMs) / 1000UL;
      if (secondsSinceFired >= CHECKIN_GRACE_SEC) {
        Serial.println("Check-in grace period elapsed -> escalating.");
        g_alertLevel = ALERT_ESCALATED;
      }
      break;
    }

    case ALERT_NORMAL:
    case ALERT_HIGH_HEAT:
    default: {
      if (secondsSinceCheckin >= g_reminderIntervalSec) {
        Serial.println("Water-reminder timer fired -> reminder_due.");
        g_alertLevel = ALERT_REMINDER_DUE;
        g_reminderFiredMs = nowMs;
      } else {
        g_alertLevel = (g_heatIndexC >= HIGH_HEAT_THRESHOLD_C) ? ALERT_HIGH_HEAT : ALERT_NORMAL;
      }
      break;
    }
  }
}

// Buzzer (GPIO17) + 3-tier status LEDs (blue/yellow/red) — non-blocking, driven every loop().
static void updateAlertOutputs(unsigned long nowMs) {
  switch (g_alertLevel) {
    case ALERT_NORMAL:
      digitalWrite(PIN_LED, LOW);
      digitalWrite(PIN_LED_YELLOW, LOW);
      digitalWrite(PIN_LED_BLUE, HIGH);
      ledcWrite(PIN_BUZZER, 0);
      break;

    case ALERT_HIGH_HEAT:
      // Steady yellow LED as a heat warning; no sound yet.
      digitalWrite(PIN_LED, LOW);
      digitalWrite(PIN_LED_YELLOW, HIGH);
      digitalWrite(PIN_LED_BLUE, LOW);
      ledcWrite(PIN_BUZZER, 0);
      break;

    case ALERT_REMINDER_DUE: {
      // Slow blink/beep (~1s period) — reminder prompt active.
      bool phaseOn = ((nowMs / 500UL) % 2UL) == 0UL;
      digitalWrite(PIN_LED, phaseOn ? HIGH : LOW);
      digitalWrite(PIN_LED_YELLOW, LOW);
      digitalWrite(PIN_LED_BLUE, LOW);
      ledcWrite(PIN_BUZZER, phaseOn ? BUZZER_PWM_DUTY_ON : 0);
      break;
    }

    case ALERT_ESCALATED: {
      // Fast blink/beep (~300ms period) — persistent alarm until check-in.
      bool phaseOn = ((nowMs / 150UL) % 2UL) == 0UL;
      digitalWrite(PIN_LED, phaseOn ? HIGH : LOW);
      digitalWrite(PIN_LED_YELLOW, LOW);
      digitalWrite(PIN_LED_BLUE, LOW);
      ledcWrite(PIN_BUZZER, phaseOn ? BUZZER_PWM_DUTY_ON : 0);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// HTTP POST to backend
// ---------------------------------------------------------------------------
static void postReading(unsigned long nowMs, unsigned long secondsSinceCheckin) {
  if (!g_haveReading) {
    Serial.println("Skipping POST: no current valid DHT20 reading.");
    return;
  }
  if (!g_deploymentConfigReady) {
    Serial.println("Skipping POST: deployment configuration is still placeholder-only.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping POST: WiFi not connected.");
    return;
  }

  // Hand-built JSON: field names/order/types must match the backend's
  // POST /api/readings body exactly.
  char body[320];
  int written = snprintf(body, sizeof(body),
    "{"
    "\"deviceId\":\"%s\","
    "\"uptimeMs\":%lu,"
    "\"tempC\":%.2f,"
    "\"humidityPct\":%.2f,"
    "\"heatIndexC\":%.2f,"
    "\"alertLevel\":\"%s\","
    "\"checkinPressed\":%s,"
    "\"secondsSinceLastCheckin\":%lu,"
    "\"reminderIntervalSec\":%lu"
    "}",
    DEVICE_ID,
    (unsigned long)nowMs,
    g_tempC,
    g_humidityPct,
    g_heatIndexC,
    alertLevelToString(g_alertLevel),
    g_checkinPressedEdge ? "true" : "false",
    secondsSinceCheckin,
    g_reminderIntervalSec
  );

  if (written < 0 || written >= (int)sizeof(body)) {
    Serial.println("JSON body build error (truncated) - skipping POST.");
    return;
  }

  String url = String("http://") + SERVER_HOST + ":" + String(SERVER_PORT) + "/api/readings";

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);

  int httpCode = http.POST((uint8_t *)body, strlen(body));
  if (httpCode > 0) {
    Serial.printf("POST /api/readings -> HTTP %d\n", httpCode);
  } else {
    Serial.printf("POST /api/readings failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();

  // checkinPressed is edge-triggered: true on exactly one POST, then cleared.
  g_checkinPressedEdge = false;
}

// ---------------------------------------------------------------------------
// setup() / loop()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("Construction Site Worker Safety Monitor - booting");
#ifdef FIRMWARE_BUILD_ID
  Serial.printf("FIRMWARE_BUILD_ID=%s\n", FIRMWARE_BUILD_ID);
#else
  Serial.println("FIRMWARE_BUILD_ID=UNSTAMPED");
#endif

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  digitalWrite(PIN_LED_YELLOW, LOW);
  pinMode(PIN_LED_BLUE, OUTPUT);
  digitalWrite(PIN_LED_BLUE, LOW);
  ledcAttach(PIN_BUZZER, BUZZER_PWM_FREQ_HZ, BUZZER_PWM_RESOLUTION_BITS);
  ledcWrite(PIN_BUZZER, 0);

  pinMode(RIGHT_BUTTON, INPUT);

  displayInit(); // no-op when ENABLE_DISPLAY == 0

  if (initializeI2CBus()) {
    initializeSensor();
  } else {
    g_sensorReady = false;
    g_lastSensorInitAttemptMs = millis();
  }

  g_deploymentConfigReady = deploymentConfigIsReady();
  if (g_deploymentConfigReady) {
    connectWiFi();
  } else {
    Serial.println("[BLOCKED] Deployment config still contains WiFi/server placeholders.");
    Serial.println("          Local sensor and alert safety logic remains active; network is disabled.");
  }

  // Baseline the check-in timer at boot so the first reminder is measured
  // from power-on, not from an uninitialized value.
  unsigned long now = millis();
  g_lastCheckinMs = now;

  // Take one sample only after the bus and sensor have passed preflight.
  if (g_sensorReady) {
    SensorSampleResult sampleResult = sampleSensor();
    if (sampleResult == SENSOR_SAMPLE_OK) {
      updateAlertState(now);
    } else if (sampleResult == SENSOR_SAMPLE_FAULT) {
      g_sensorReady = false;
      g_lastSensorInitAttemptMs = now;
      releaseI2CBus();
      Serial.println("DHT20 marked unavailable; bounded Wire rebuild enabled.");
    }
  }
  g_lastSampleMs = now;

  Serial.println("Setup complete.");
}

void loop() {
  unsigned long now = millis();

  // 1. Poll check-in button every iteration (cheap, needs fast polling for
  //    reliable debounce) and handle any press immediately.
  if (pollButtonPressedEdge()) {
    onButtonPress(now);
  }

  // 2. Keep WiFi alive in the background; never blocks sampling/alerts.
  maintainWiFi();

  // 3. Re-evaluate the alert-level state machine every iteration so grace
  //    periods / reminder timers are checked promptly, independent of the
  //    (slower) DHT20 sample cadence.
  updateAlertState(now);

  // 4. Drive buzzer/LED from current alert state (non-blocking patterns).
  updateAlertOutputs(now);

  // 5. Retry a failed DHT20 preflight at a bounded cadence. This avoids
  //    repeatedly stimulating a faulted shared I2C bus.
  if (!g_sensorReady &&
      now - g_lastSensorInitAttemptMs >= SENSOR_RETRY_INTERVAL_MS) {
    initializeSensor();
  }

  // 6. Sample DHT20 + POST only after a current sample succeeds.
  if (now - g_lastSampleMs >= SAMPLE_INTERVAL_MS) {
    g_lastSampleMs = now;
    if (g_sensorReady) {
      SensorSampleResult sampleResult = sampleSensor();
      if (sampleResult == SENSOR_SAMPLE_OK) {
        // Recompute state immediately after a fresh sample so a newly-crossed
        // heat threshold or reminder firing is reflected before this cycle's POST.
        updateAlertState(now);

        unsigned long secondsSinceCheckin = (now - g_lastCheckinMs) / 1000UL;
        postReading(now, secondsSinceCheckin);
      } else if (sampleResult == SENSOR_SAMPLE_FAULT) {
        g_sensorReady = false;
        g_lastSensorInitAttemptMs = now;
        releaseI2CBus();
        Serial.println("DHT20 marked unavailable; bounded Wire rebuild enabled.");
      }
    }
  }

  // 7. Refresh the status screen (no-op when ENABLE_DISPLAY == 0).
  unsigned long secondsSinceCheckin = (now - g_lastCheckinMs) / 1000UL;
  displayUpdate(WiFi.status() == WL_CONNECTED, secondsSinceCheckin);

  delay(20); // small yield; keeps button polling responsive without busy-spinning the CPU
}
