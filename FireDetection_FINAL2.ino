// ============================================================================
//  INTELLIGENT FIRE DETECTION SYSTEM
//  Platform  : Arduino Uno R3 (ATmega328P)
//  Version   : 3.0.0  — FINAL
// ----------------------------------------------------------------------------
//  - DHT11 temperature & humidity
//  - I2C LCD 16x2  (address 0x27, no RTC)
//  - MQ-2 smoke / gas sensor
//  - IR Flame sensor (Active LOW)
//  - Passive buzzer, Relay module (Active LOW)
//  - Traffic light LEDs: Green / Yellow / Red
//  - GREEN LED stays ON at Level 0 (Safe) and Level 1 (Warning)
//    → turns OFF only when Level 2 (Danger) fires
//  - Zero delay() in main loop — all timing via millis()
// ============================================================================

// ── Libraries ────────────────────────────────────────────────────────────────
#include <Wire.h>
#include <LiquidCrystal_I2C.h>   // Frank de Brabander — Library Manager
#include <DHT.h>                  // Adafruit DHT sensor library

// ── Pin Map ───────────────────────────────────────────────────────────────────
#define PIN_DHT         4    // DHT11 data line
#define PIN_SMOKE       A0   // MQ-2  analog output
#define PIN_FLAME       2    // IR flame sensor  — Active LOW
#define PIN_BUTTON      7    // Mode toggle      — Active LOW, INPUT_PULLUP
#define PIN_BUZZER      5    // Passive buzzer   — PWM, tone()
#define PIN_RELAY       6    // Relay / fan      — Active LOW
#define PIN_LED_GREEN   8    // Green  LED  (Safe  +  Warning)
#define PIN_LED_YELLOW  9    // Yellow LED  (Warning only)
#define PIN_LED_RED     10   // Red    LED  (Danger — blinks)

// ── Sensor thresholds ─────────────────────────────────────────────────────────
#define DHT_TYPE        DHT11
#define TEMP_THRESHOLD  45.0f   // °C  — above this = +1 fusion point
#define SMOKE_THRESHOLD 350     // ADC — above this = +1 fusion point

// ── Timing constants (ms) ─────────────────────────────────────────────────────
#define INTERVAL_SENSOR  2000UL   // sensor read + LCD refresh
#define INTERVAL_SERIAL  15000UL  // serial status log
#define INTERVAL_BLINK   500UL    // red LED + LCD row-0 flash period
#define INTERVAL_TONE_A  300UL    // buzzer high-note duration
#define INTERVAL_TONE_B  300UL    // buzzer low-note duration
#define DEBOUNCE_MS      50UL     // button debounce window

// ── Buzzer siren frequencies ──────────────────────────────────────────────────
#define TONE_HIGH  1800   // Hz
#define TONE_LOW    900   // Hz

// ── LCD ───────────────────────────────────────────────────────────────────────
#define LCD_ADDR  0x27   // try 0x3F if screen stays blank
#define LCD_COLS  16
#define LCD_ROWS  2

// ── Objects ───────────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
DHT               dht(PIN_DHT, DHT_TYPE);

// ── State structures ──────────────────────────────────────────────────────────
struct SensorData {
  float   temperature;    // °C   (NaN on DHT read failure)
  float   humidity;       // %RH  (NaN on DHT read failure)
  int     smokeRaw;       // 0–1023 ADC
  bool    flameDetected;  // true = flame present (Active LOW handled in code)
};

struct FusionResult {
  uint8_t points;         // 0–3 accumulated points
  bool    tempHigh;
  bool    smokeHigh;
  bool    flameOn;
};

struct SystemState {
  SensorData   sensors;
  FusionResult fusion;
  uint8_t      alarmLevel;   // 0 = Safe | 1 = Warning | 2 = Danger
  bool         simMode;      // true = simulation (button toggled)
  bool         relayActive;
};

SystemState sys;

// ── Timing trackers ───────────────────────────────────────────────────────────
static unsigned long lastSensorTick  = 0;
static unsigned long lastSerialTick  = 0;
static unsigned long lastBlinkTick   = 0;
static unsigned long lastToneTick    = 0;
static unsigned long lastButtonCheck = 0;

// ── Loop-state flags ──────────────────────────────────────────────────────────
static bool lcdBlinkState  = false;
static bool tonePhaseHigh  = true;
static bool buttonLastState = HIGH;

// ── Forward declarations ──────────────────────────────────────────────────────
void    readSensors();
void    processFusion();
void    updateDisplay();
void    handleAlarms();
void    checkButton();
void    checkWifi();
void    setLEDs(uint8_t level);
void    setRelay(bool active);
void    printLcdRow1();
void    logToSerial(const String& msg);
void    logToSerial(const __FlashStringHelper* msg);
String  uptimeStr();
String  levelName(uint8_t level);

// ============================================================================
//  SETUP
// ============================================================================
void setup() {
  Serial.begin(9600);
  Serial.println(F("=== Fire Detection System v3.0 — FINAL ==="));

  // Output pins
  pinMode(PIN_BUZZER,    OUTPUT);
  pinMode(PIN_RELAY,     OUTPUT);
  pinMode(PIN_LED_GREEN,  OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED,    OUTPUT);

  // Input pins
  pinMode(PIN_FLAME,  INPUT);        // IR sensor board has its own pull-up
  pinMode(PIN_BUTTON, INPUT_PULLUP); // Active LOW — no external resistor needed

  // Safe startup state
  noTone(PIN_BUZZER);
  setRelay(false);
  setLEDs(0);   // Green ON immediately at boot

  // LCD
  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("  Fire Detector "));
  lcd.setCursor(0, 1); lcd.print(F("  Starting up.. "));
  delay(1500);   // one-time splash — not in main loop
  lcd.clear();

  // DHT11
  dht.begin();

  // Zero the state struct
  memset(&sys, 0, sizeof(sys));
  sys.sensors.temperature = NAN;
  sys.sensors.humidity    = NAN;

  logToSerial(F("Boot complete. Green LED on. Monitoring started."));
}

// ============================================================================
//  MAIN LOOP — non-blocking
// ============================================================================
void loop() {
  unsigned long now = millis();

  // 1. Button debounce + sim-mode toggle
  checkButton();

  // 2. Sensor pipeline every 2 s
  if (now - lastSensorTick >= INTERVAL_SENSOR) {
    lastSensorTick = now;
    readSensors();
    processFusion();
    updateDisplay();
    sendSensorData();
  }

  // 3. Alarm outputs — runs every iteration for smooth siren + blink
  handleAlarms();

  // 4. Periodic serial status every 15 s
  if (now - lastSerialTick >= INTERVAL_SERIAL) {
    lastSerialTick = now;
    String msg = F("STATUS | Lvl:");
    msg += levelName(sys.alarmLevel);
    msg += F(" | T:");
    msg += isnan(sys.sensors.temperature) ? String(F("ERR")) : String(sys.sensors.temperature, 1) + String(F("C"));
    msg += F(" | H:");
    msg += isnan(sys.sensors.humidity)    ? String(F("ERR")) : String(sys.sensors.humidity, 1)    + String(F("%"));
    msg += F(" | Smoke:");  msg += sys.sensors.smokeRaw;
    msg += F(" | Flame:");  msg += sys.sensors.flameDetected ? F("YES") : F("NO");
    msg += F(" | Sim:");    msg += sys.simMode ? F("ON") : F("OFF");
    logToSerial(msg);
  }

  // 5. Wi-Fi stub (future ESP-01)
  checkWifi();
}

// ============================================================================
//  readSensors()
//  Populates sys.sensors with live or simulated data.
//  Active LOW: flame sensor LOW = fire present.
// ============================================================================
void readSensors() {
  if (sys.simMode) {
    sys.sensors.temperature   = 60.0f;   // > 45°C threshold
    sys.sensors.humidity      = 30.0f;
    sys.sensors.smokeRaw      = 500;     // > 350 threshold
    sys.sensors.flameDetected = true;
    return;
  }

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    logToSerial(F("WARN: DHT11 read failed — keeping last valid data."));
  } else {
    sys.sensors.temperature = t;
    sys.sensors.humidity    = h;
  }

  sys.sensors.smokeRaw      = analogRead(PIN_SMOKE);
  sys.sensors.flameDetected = (digitalRead(PIN_FLAME) == LOW);  // Active LOW
}

// ============================================================================
//  processFusion()
//  Point system: each threshold met = +1 point (max 3).
//  0 pts → SAFE | 1 pt → WARNING | ≥2 pts → DANGER
// ============================================================================
void processFusion() {
  FusionResult&     f = sys.fusion;
  const SensorData& s = sys.sensors;

  f.tempHigh  = (!isnan(s.temperature)) && (s.temperature > TEMP_THRESHOLD);
  f.smokeHigh = (s.smokeRaw > SMOKE_THRESHOLD);
  f.flameOn   = s.flameDetected;
  f.points    = (uint8_t)f.tempHigh + (uint8_t)f.smokeHigh + (uint8_t)f.flameOn;

  uint8_t prev = sys.alarmLevel;
  if      (f.points == 0) sys.alarmLevel = 0;
  else if (f.points == 1) sys.alarmLevel = 1;
  else                    sys.alarmLevel = 2;

  if (sys.alarmLevel != prev) {
    String msg = F("TRANSITION -> ");
    msg += levelName(sys.alarmLevel);
    msg += F(" (pts="); msg += f.points;
    msg += F(") T=");
    msg += isnan(s.temperature) ? String(F("ERR")) : String(s.temperature, 1);
    msg += F("C Smoke="); msg += s.smokeRaw;
    msg += F(" Flame=");  msg += s.flameDetected ? F("YES") : F("NO");
    logToSerial(msg);
  }
}

// ============================================================================
//  updateDisplay()
// ----------------------------------------------------------------------------
//  LCD layout — 16 cols × 2 rows:
//
//  Level 0  Row0: "SAFE  T:24.5C   "   Level 1  Row0: "!WARN T:46.1C   "
//           Row1: "Smoke:87  Fl:NO "            Row1: "Smoke:87  Fl:NO "
//
//  Level 2  Row0: "*** FIRE ***    "  ← flashes in handleAlarms()
//           Row1: "Smoke:500 Fl:YES"  ← always visible
//
//  LED rule:
//    GREEN  = ON  at Level 0 and Level 1  (system healthy / advisory only)
//    GREEN  = OFF at Level 2 (full danger)
//    YELLOW = ON  at Level 1 only
//    RED    = blinks at Level 2 (managed in handleAlarms)
// ============================================================================
void updateDisplay() {
  setLEDs(sys.alarmLevel);

  // NOTE: Arduino Uno (AVR) snprintf does NOT support %f — outputs '?' instead.
  // Use dtostrf(value, totalWidth, decimalPlaces, charBuffer) for floats on AVR.
  char tempBuf[7];
  if (isnan(sys.sensors.temperature))
    strncpy(tempBuf, "  ---", sizeof(tempBuf));
  else
    dtostrf(sys.sensors.temperature, 5, 1, tempBuf);  // e.g. " 24.5"

  char row0[17];

  if (sys.alarmLevel == 0) {
    snprintf(row0, sizeof(row0), "SAFE  T:%sC  ", tempBuf);
    lcd.setCursor(0, 0);
    lcd.print(row0);

  } else if (sys.alarmLevel == 1) {
    snprintf(row0, sizeof(row0), "!WARN T:%sC  ", tempBuf);
    lcd.setCursor(0, 0);
    lcd.print(row0);
  }
  // Level 2 row 0 is handled inside handleAlarms() for the flash effect

  printLcdRow1();   // row 1 always shows live smoke + flame
}

// ============================================================================
//  printLcdRow1()  —  "Smoke:NNN Fl:YES"
// ============================================================================
void printLcdRow1() {
  char row1[17];
  snprintf(row1, sizeof(row1), "Smoke:%-3d Fl:%-3s",
           sys.sensors.smokeRaw,
           sys.sensors.flameDetected ? "YES" : "NO ");
  lcd.setCursor(0, 1);
  lcd.print(row1);
}

// ============================================================================
//  handleAlarms()
//  Called every loop() iteration. Uses millis() for all timing.
//  Level 0/1 → buzzer off, relay off.
//  Level 2   → flashing red LED + LCD row 0, oscillating buzzer, relay on.
//              Relay is BLOCKED in simulation mode (safety constraint).
// ============================================================================
void handleAlarms() {
  unsigned long now = millis();

  if (sys.alarmLevel < 2) {
    noTone(PIN_BUZZER);
    setRelay(false);
    return;
  }

  // ── DANGER ───────────────────────────────────────────────────────────────
  setRelay(!sys.simMode);   // relay ON in real mode only

  // Red LED + LCD row 0 flash at INTERVAL_BLINK
  if (now - lastBlinkTick >= INTERVAL_BLINK) {
    lastBlinkTick = now;
    lcdBlinkState = !lcdBlinkState;

    digitalWrite(PIN_LED_RED, lcdBlinkState ? HIGH : LOW);

    lcd.setCursor(0, 0);
    lcd.print(lcdBlinkState ? F("*** FIRE ***    ") : F("                "));

    printLcdRow1();   // row 1 stays live during blink
  }

  // Two-tone oscillating siren
  unsigned long dur = tonePhaseHigh ? INTERVAL_TONE_A : INTERVAL_TONE_B;
  if (now - lastToneTick >= dur) {
    lastToneTick  = now;
    tonePhaseHigh = !tonePhaseHigh;
    tone(PIN_BUZZER, tonePhaseHigh ? TONE_HIGH : TONE_LOW);
  }
}

// ============================================================================
//  checkButton()  — non-blocking 50 ms debounce, falling-edge detection
// ============================================================================
void checkButton() {
  unsigned long now     = millis();
  bool          reading = digitalRead(PIN_BUTTON);

  if (now - lastButtonCheck < DEBOUNCE_MS) return;

  if (reading == LOW && buttonLastState == HIGH) {
    lastButtonCheck = now;
    sys.simMode = !sys.simMode;

    logToSerial(sys.simMode ? F("SIM MODE ON  — relay blocked, fire values injected.")
                            : F("SIM MODE OFF — returning to real sensors."));

    if (!sys.simMode) {
      noTone(PIN_BUZZER);
      setRelay(false);
      lcd.clear();
    }
  }
  buttonLastState = reading;
}

// ============================================================================
//  checkWifi()  — STUB for future ESP-01 / AT-command integration
// ============================================================================
void checkWifi() {
  // TODO:
  //   1. SoftwareSerial wifiSerial(11, 12) to ESP-01
  //   2. AT+CIPSTART → MQTT broker / HTTP endpoint
  //   3. AT+CIPSEND  → JSON payload with alarmLevel + sensor values
  //   Implement as a non-blocking state machine — no delay() allowed.
}

// ============================================================================
//  HELPERS
// ============================================================================

/**
 * setLEDs()  —  controls all three traffic-light LEDs.
 *
 *   Level 0  SAFE    : GREEN on  | YELLOW off | RED off
 *   Level 1  WARNING : GREEN on  | YELLOW on  | RED off   ← GREEN stays on
 *   Level 2  DANGER  : GREEN off | YELLOW off | RED blinks (managed in handleAlarms)
 */
void setLEDs(uint8_t level) {
  // Green ON for level 0 AND 1 — turns off only at level 2
  digitalWrite(PIN_LED_GREEN,  (level < 2)  ? HIGH : LOW);
  // Yellow ON only at level 1
  digitalWrite(PIN_LED_YELLOW, (level == 1) ? HIGH : LOW);
  // Red is managed by handleAlarms() blink; force off here for levels 0/1
  if (level < 2) digitalWrite(PIN_LED_RED, LOW);
}

/**
 * setRelay()  —  Active LOW module: LOW = coil ON = fan running.
 */
void setRelay(bool active) {
  digitalWrite(PIN_RELAY, active ? LOW : HIGH);
  sys.relayActive = active;
}

/**
 * uptimeStr()  —  returns millis-based "HH:MM:SS" uptime string.
 */
String uptimeStr() {
  unsigned long s = millis() / 1000UL;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           s / 3600, (s % 3600) / 60, s % 60);
  return String(buf);
}

void logToSerial(const String& msg) {
  Serial.print(F("["));
  Serial.print(uptimeStr());
  Serial.print(F("] "));
  Serial.println(msg);
}

void logToSerial(const __FlashStringHelper* msg) {
  logToSerial(String(msg));
}

String levelName(uint8_t level) {
  switch (level) {
    case 0:  return String(F("SAFE"));
    case 1:  return String(F("WARNING"));
    default: return String(F("DANGER"));
  }
}

void sendSensorData() {
  Serial.print("{");
  Serial.print("\"temperature\":");
  Serial.print(isnan(sys.sensors.temperature) ? "null" : String(sys.sensors.temperature, 1));
  Serial.print(",\"smoke\":");
  Serial.print(sys.sensors.smokeRaw);
  Serial.print(",\"flame\":");
  Serial.print(sys.sensors.flameDetected ? "true" : "false");
  Serial.print(",\"level\":");
  Serial.print(sys.alarmLevel);
  Serial.print(",\"pts\":");
  Serial.print(sys.fusion.points);
  Serial.print(",\"sim\":");
  Serial.print(sys.simMode ? "true" : "false");
  Serial.print(",\"uptime\":\"");
  Serial.print(uptimeStr());
  Serial.println("\"}");
}

// ============================================================================
//  END OF FILE
// ============================================================================
