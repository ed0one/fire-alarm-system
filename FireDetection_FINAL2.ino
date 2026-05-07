// ============================================================================
//  INTELLIGENT FIRE DETECTION SYSTEM — ESP32 Edition
//  Platform  : ESP32 (38-pin DevKit or WROOM-32)
//  Version   : 4.0.1
// ----------------------------------------------------------------------------
//  NEW in v4.0 (ESP32):
//    - Built-in WiFi with network scan + credential setup via Serial Monitor
//    - Credentials saved to NVS flash (Preferences) — survives power cycles
//    - Fixed buzzer: 3-phase siren (HIGH → LOW → SILENT) — no more stuck tone
//    - Smoke threshold: 500 ADC (warning at >500)
//    - Sim mode smoke corrected to 600 (above 500 threshold)
//    - sendSensorData() sends JSON to Serial for web dashboard bridge
//  FIXED in v4.0.1:
//    - PIN_LED_GREEN moved to GPIO 27 (GPIO 17 not exposed on this board)
//    - Corrected ADC2 pin comment — GPIO 27 used as digital output only, no conflict
//    - Corrected DHT11 pin comment — GPIO 4 is digital, ADC2 note was misleading
//    - Removed stale comment about WIFI reconfigure in checkButton() body
// ============================================================================

// ── Libraries ────────────────────────────────────────────────────────────────
#include <Wire.h>
#include <LiquidCrystal_I2C.h>   // Frank de Brabander
#include <DHT.h>                  // Adafruit DHT sensor library
#include <WiFi.h>                 // ESP32 built-in
#include <Preferences.h>          // ESP32 NVS key-value flash storage

// ── ESP32 Pin Map ─────────────────────────────────────────────────────────────
// IMPORTANT: GPIO 6–11 are reserved for SPI flash — never use them.
// ADC2 pins conflict with WiFi only when used as analog inputs (analogRead).
// Using ADC2-capable pins as digital I/O is safe regardless of WiFi state.
#define PIN_DHT         4    // DHT11 data  (digital input — ADC2 note does not apply)
#define PIN_SMOKE       34   // MQ-2 analog (ADC1_CH6 — input only, no pull-up)
#define PIN_FLAME       13   // IR flame sensor — Active LOW
#define PIN_BUTTON      15   // Mode toggle — Active LOW, INPUT_PULLUP
#define PIN_BUZZER      25   // Passive buzzer — LEDC PWM channel 0
#define PIN_RELAY       16   // Relay / fan — Active LOW
#define PIN_LED_GREEN   27   // Green  LED (Safe + Warning) — GPIO 17 not on this board
#define PIN_LED_YELLOW  18   // Yellow LED (Warning only)
#define PIN_LED_RED     19   // Red    LED (Danger — blinks)

// I2C default on ESP32: SDA = GPIO 21, SCL = GPIO 22 (no change needed)

// ── Sensor thresholds ─────────────────────────────────────────────────────────
#define DHT_TYPE         DHT11
#define TEMP_THRESHOLD   45.0f  // °C
#define SMOKE_THRESHOLD  500    // ADC 0–4095 (ESP32 is 12-bit, Uno was 10-bit)
                                // MQ-2 at rest ≈ 100–300, smoke ≈ 500–2000

// ── Timing constants (ms) ─────────────────────────────────────────────────────
#define INTERVAL_SENSOR   2000UL
#define INTERVAL_SERIAL   15000UL
#define INTERVAL_BLINK    500UL
#define DEBOUNCE_MS       50UL
#define WIFI_TIMEOUT_MS   15000UL  // 15 s connection timeout

// ── Buzzer 3-phase siren ──────────────────────────────────────────────────────
// Phase 0: HIGH tone (400 ms) → Phase 1: LOW tone (300 ms) → Phase 2: silence (300 ms)
// This creates a proper pulsing alarm instead of a continuous screech.
#define TONE_HIGH        1800   // Hz
#define TONE_LOW          600   // Hz
#define SIREN_PHASE_0_MS  400UL // high note duration
#define SIREN_PHASE_1_MS  300UL // low  note duration
#define SIREN_PHASE_2_MS  300UL // silence duration

// ── ESP32 LEDC (PWM) for tone() ───────────────────────────────────────────────
#define LEDC_CHANNEL     0
#define LEDC_RESOLUTION  8    // 8-bit (0–255)

// ── LCD ───────────────────────────────────────────────────────────────────────
#define LCD_ADDR  0x3F   // common address; try 0x27 if blank
#define LCD_COLS  16
#define LCD_ROWS  2

// ── NVS namespace ─────────────────────────────────────────────────────────────
#define NVS_NAMESPACE  "firedet"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "pass"

// ============================================================================
//  OBJECTS
// ============================================================================
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
DHT               dht(PIN_DHT, DHT_TYPE);
Preferences       prefs;

// ============================================================================
//  STATE STRUCTURES
// ============================================================================
struct SensorData {
  float   temperature;
  float   humidity;
  int     smokeRaw;
  bool    flameDetected;
};

struct FusionResult {
  uint8_t points;
  bool    tempHigh;
  bool    smokeHigh;
  bool    flameOn;
};

struct SystemState {
  SensorData   sensors;
  FusionResult fusion;
  uint8_t      alarmLevel;
  bool         simMode;
  bool         relayActive;
  bool         wifiConnected;
  String       wifiSSID;
  String       localIP;
};

SystemState sys;

// ── Timing trackers ───────────────────────────────────────────────────────────
static unsigned long lastSensorTick  = 0;
static unsigned long lastSerialTick  = 0;
static unsigned long lastBlinkTick   = 0;
static unsigned long lastToneTick    = 0;
static unsigned long lastButtonCheck = 0;
static unsigned long lastJsonTick    = 0;

// ── Siren state (0=high, 1=low, 2=silent) ─────────────────────────────────────
static uint8_t sirenPhase     = 0;
static bool    lcdBlinkState  = false;
static bool    buttonLastState = HIGH;

// ── Forward declarations ──────────────────────────────────────────────────────
void     readSensors();
void     processFusion();
void     updateDisplay();
void     handleAlarms();
void     checkButton();
void     setupWifi();
void     tryConnectWifi(const String& ssid, const String& pass);
void     scanAndConfigureWifi();
void     setLEDs(uint8_t level);
void     setRelay(bool active);
void     buzzerTone(uint32_t freq);
void     buzzerOff();
void     printLcdRow1();
void     sendSensorData();
void     logToSerial(const String& msg);
void     logToSerial(const __FlashStringHelper* msg);
String   uptimeStr();
String   levelName(uint8_t level);

// ============================================================================
//  SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n=== Fire Detection System v4.0.1 — ESP32 ==="));

  // ── Output pins ───────────────────────────────────────────────────────────
  pinMode(PIN_RELAY,      OUTPUT);
  pinMode(PIN_LED_GREEN,  OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED,    OUTPUT);

  // ── Input pins ────────────────────────────────────────────────────────────
  pinMode(PIN_FLAME,  INPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_SMOKE,  INPUT);  // GPIO 34 — input only

  // ── LEDC (buzzer PWM) — ESP32 Arduino core v3.x API ─────────────────────
  // ledcAttach(pin, freq, resolution) replaces the old ledcSetup/ledcAttachPin pair.
  ledcAttach(PIN_BUZZER, 1000, LEDC_RESOLUTION);
  buzzerOff();

  // ── Safe startup state ───────────────────────────────────────────────────
  setRelay(false);
  setLEDs(0);

  // ── I2C + LCD ─────────────────────────────────────────────────────────────
  Wire.begin(21, 22);   // SDA=21, SCL=22 (ESP32 defaults)
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Fire Det v4.0  "));
  lcd.setCursor(0, 1); lcd.print(F("ESP32 Starting."));

  // ── DHT11 ────────────────────────────────────────────────────────────────
  dht.begin();

  // ── Zero state ───────────────────────────────────────────────────────────
  memset(&sys, 0, sizeof(sys));
  sys.sensors.temperature = NAN;
  sys.sensors.humidity    = NAN;
  sys.wifiConnected       = false;
  sys.localIP             = "Not connected";
  sys.wifiSSID            = "";

  // ── WiFi setup ────────────────────────────────────────────────────────────
  setupWifi();

  delay(500);
  lcd.clear();
  logToSerial(F("Boot complete. Monitoring started."));
}

// ============================================================================
//  MAIN LOOP
// ============================================================================
void loop() {
  unsigned long now = millis();

  checkButton();

  if (now - lastSensorTick >= INTERVAL_SENSOR) {
    lastSensorTick = now;
    readSensors();
    processFusion();
    updateDisplay();
  }

  handleAlarms();

  // JSON data for web dashboard bridge (every 2 s)
  if (now - lastJsonTick >= 2000UL) {
    lastJsonTick = now;
    sendSensorData();
  }

  // Plain-text status log every 15 s
  if (now - lastSerialTick >= INTERVAL_SERIAL) {
    lastSerialTick = now;
    String msg = F("STATUS | Lvl:");
    msg += levelName(sys.alarmLevel);
    msg += F(" | T:");
    msg += isnan(sys.sensors.temperature)
             ? String(F("ERR"))
             : String(sys.sensors.temperature, 1) + String(F("C"));
    msg += F(" | H:");
    msg += isnan(sys.sensors.humidity)
             ? String(F("ERR"))
             : String(sys.sensors.humidity, 1) + String(F("%"));
    msg += F(" | Smoke:"); msg += sys.sensors.smokeRaw;
    msg += F(" | Flame:"); msg += sys.sensors.flameDetected ? F("YES") : F("NO");
    msg += F(" | WiFi:");  msg += sys.wifiConnected ? sys.wifiSSID : String(F("OFF"));
    msg += F(" | IP:");    msg += sys.localIP;
    logToSerial(msg);
  }
}

// ============================================================================
//  setupWifi()
// ----------------------------------------------------------------------------
//  1. Load saved credentials from NVS.
//  2. Try to connect with a 15-second timeout.
//  3. If connection fails OR no credentials saved → enter interactive setup.
//  4. Interactive setup: scan networks, user picks SSID + types password
//     via Serial Monitor (set to "No line ending" or "Newline").
// ============================================================================
void setupWifi() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("WiFi Setup...  "));

  // Load saved credentials
  prefs.begin(NVS_NAMESPACE, true);  // read-only
  String savedSSID = prefs.getString(NVS_KEY_SSID, "");
  String savedPass = prefs.getString(NVS_KEY_PASS, "");
  prefs.end();

  if (savedSSID.length() > 0) {
    Serial.print(F("[WiFi] Saved network found: "));
    Serial.println(savedSSID);
    lcd.setCursor(0, 1);
    lcd.print(savedSSID.substring(0, 16));

    tryConnectWifi(savedSSID, savedPass);

    if (sys.wifiConnected) return;

    Serial.println(F("[WiFi] Saved credentials failed. Starting scan..."));
  } else {
    Serial.println(F("[WiFi] No saved credentials. Starting scan..."));
  }

  // No saved creds or connection failed — interactive setup
  scanAndConfigureWifi();
}

// ============================================================================
//  tryConnectWifi()
// ============================================================================
void tryConnectWifi(const String& ssid, const String& pass) {
  Serial.print(F("[WiFi] Connecting to: "));
  Serial.println(ssid);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Connecting...  "));
  lcd.setCursor(0, 1);
  lcd.print(ssid.substring(0, 16));

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  uint8_t dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println(F("\n[WiFi] Connection timed out."));
      WiFi.disconnect();
      sys.wifiConnected = false;
      return;
    }
    delay(500);
    Serial.print('.');
    // Animate LCD row 1
    lcd.setCursor(dots % 16, 1);
    lcd.print('.');
    dots++;
  }

  sys.wifiConnected = true;
  sys.wifiSSID      = ssid;
  sys.localIP       = WiFi.localIP().toString();

  Serial.println();
  Serial.print(F("[WiFi] Connected! IP: "));
  Serial.println(sys.localIP);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("WiFi Connected!"));
  lcd.setCursor(0, 1); lcd.print(sys.localIP.substring(0, 16));
  delay(2000);
}

// ============================================================================
//  scanAndConfigureWifi()
// ----------------------------------------------------------------------------
//  Scans for networks, prints a numbered list, waits for user to type the
//  network number and then the password in Serial Monitor.
//  Saves to NVS on successful connection.
// ============================================================================
void scanAndConfigureWifi() {
  Serial.println(F("\n╔═══════════════════════════════════╗"));
  Serial.println(F("║     WiFi Network Configuration    ║"));
  Serial.println(F("╚═══════════════════════════════════╝"));
  Serial.println(F("Open Serial Monitor at 115200 baud."));
  Serial.println(F("Scanning for networks...\n"));

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Scanning WiFi.."));

  int n = WiFi.scanNetworks();

  if (n == 0) {
    Serial.println(F("[WiFi] No networks found. Skipping WiFi setup."));
    lcd.setCursor(0, 1); lcd.print(F("No networks!   "));
    delay(2000);
    return;
  }

  // Print numbered list
  Serial.println(F("Available networks:"));
  Serial.println(F("───────────────────────────────────"));
  for (int i = 0; i < n; i++) {
    Serial.print(F("  ["));
    Serial.print(i + 1);
    Serial.print(F("] "));
    Serial.print(WiFi.SSID(i));
    Serial.print(F("  (RSSI: "));
    Serial.print(WiFi.RSSI(i));
    Serial.print(F(" dBm) "));
    Serial.println(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? F("[Open]") : F("[Secured]"));
  }
  Serial.println(F("  [0] Skip WiFi setup"));
  Serial.println(F("───────────────────────────────────"));

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Check Serial   "));
  lcd.setCursor(0, 1); lcd.print(F("Pick network # "));

  // ── Wait for network number ───────────────────────────────────────────────
  Serial.print(F("\nEnter network number (1–"));
  Serial.print(n);
  Serial.println(F(") or 0 to skip: "));

  String inputNum = "";
  unsigned long waitStart = millis();
  // Wait up to 60 seconds for input
  while (millis() - waitStart < 60000UL) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (inputNum.length() > 0) break;
      } else {
        inputNum += c;
        Serial.print(c);  // echo
      }
    }
  }
  Serial.println();

  int choice = inputNum.toInt();
  if (choice < 1 || choice > n) {
    Serial.println(F("[WiFi] Skipping WiFi setup."));
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(F("WiFi Skipped   "));
    delay(1500);
    return;
  }

  String chosenSSID = WiFi.SSID(choice - 1);
  bool   isOpen     = (WiFi.encryptionType(choice - 1) == WIFI_AUTH_OPEN);

  Serial.print(F("Selected: "));
  Serial.println(chosenSSID);

  // ── Wait for password ─────────────────────────────────────────────────────
  String password = "";
  if (!isOpen) {
    Serial.print(F("Enter password for \""));
    Serial.print(chosenSSID);
    Serial.println(F("\" (then press Enter): "));

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(F("Enter Password "));
    lcd.setCursor(0, 1); lcd.print(F("in Serial Mon. "));

    waitStart = millis();
    while (millis() - waitStart < 120000UL) {  // 2-minute timeout
      if (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
          if (password.length() > 0) break;
        } else {
          password += c;
          Serial.print('*');  // mask password in terminal
        }
      }
    }
    Serial.println();
  }

  // ── Try connecting ─────────────────────────────────────────────────────────
  tryConnectWifi(chosenSSID, password);

  if (sys.wifiConnected) {
    // Save credentials to NVS
    prefs.begin(NVS_NAMESPACE, false);  // read-write
    prefs.putString(NVS_KEY_SSID, chosenSSID);
    prefs.putString(NVS_KEY_PASS, password);
    prefs.end();
    Serial.println(F("[NVS] Credentials saved to flash."));
    Serial.println(F("[NVS] They will be used automatically on next boot."));
  } else {
    Serial.println(F("[WiFi] Connection failed. Running without WiFi."));
    Serial.println(F("[WiFi] Type 'WIFI' in Serial Monitor to reconfigure."));
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(F("WiFi Failed    "));
    lcd.setCursor(0, 1); lcd.print(F("Running offline"));
    delay(2000);
  }
}

// ============================================================================
//  readSensors()
// ============================================================================
void readSensors() {
  if (sys.simMode) {
    sys.sensors.temperature   = 60.0f;
    sys.sensors.humidity      = 30.0f;
    sys.sensors.smokeRaw      = 600;   // above 500 threshold → triggers warning
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

  // ESP32 ADC is 12-bit (0–4095); MQ-2 output scales accordingly
  sys.sensors.smokeRaw      = analogRead(PIN_SMOKE);
  sys.sensors.flameDetected = (digitalRead(PIN_FLAME) == LOW);  // Active LOW
}

// ============================================================================
//  processFusion()
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
// ============================================================================
void updateDisplay() {
  setLEDs(sys.alarmLevel);

  char tempBuf[7];
  if (isnan(sys.sensors.temperature))
    strncpy(tempBuf, "  ---", sizeof(tempBuf));
  else
    dtostrf(sys.sensors.temperature, 5, 1, tempBuf);

  char row0[17];

  if (sys.alarmLevel == 0) {
    snprintf(row0, sizeof(row0), "SAFE  T:%sC  ", tempBuf);
    lcd.setCursor(0, 0); lcd.print(row0);
  } else if (sys.alarmLevel == 1) {
    snprintf(row0, sizeof(row0), "!WARN T:%sC  ", tempBuf);
    lcd.setCursor(0, 0); lcd.print(row0);
  }

  printLcdRow1();
}

// ============================================================================
//  printLcdRow1()
// ============================================================================
void printLcdRow1() {
  char row1[17];
  snprintf(row1, sizeof(row1), "Smoke:%-4d Fl:%s",
           sys.sensors.smokeRaw,
           sys.sensors.flameDetected ? "Y" : "N");
  lcd.setCursor(0, 1);
  lcd.print(row1);
}

// ============================================================================
//  handleAlarms()
// ----------------------------------------------------------------------------
//  FIXED BUZZER — 3-phase siren:
//    Phase 0 (400 ms): TONE_HIGH played
//    Phase 1 (300 ms): TONE_LOW  played
//    Phase 2 (300 ms): SILENT    — this is what was missing before
//
//  The silence gap makes the alarm pulse instead of screech continuously.
// ============================================================================
void handleAlarms() {
  unsigned long now = millis();

  if (sys.alarmLevel < 2) {
    buzzerOff();
    setRelay(false);
    sirenPhase = 0;  // reset phase so next alarm starts from HIGH
    return;
  }

  // ── DANGER ───────────────────────────────────────────────────────────────
  setRelay(!sys.simMode);

  // ── Red LED + LCD row 0 blink (500 ms) ───────────────────────────────────
  if (now - lastBlinkTick >= INTERVAL_BLINK) {
    lastBlinkTick = now;
    lcdBlinkState = !lcdBlinkState;
    digitalWrite(PIN_LED_RED, lcdBlinkState ? HIGH : LOW);
    lcd.setCursor(0, 0);
    lcd.print(lcdBlinkState ? F("*** FIRE ***    ") : F("                "));
    printLcdRow1();
  }

  // ── 3-phase siren ────────────────────────────────────────────────────────
  unsigned long phaseDur;
  switch (sirenPhase) {
    case 0: phaseDur = SIREN_PHASE_0_MS; break;
    case 1: phaseDur = SIREN_PHASE_1_MS; break;
    default: phaseDur = SIREN_PHASE_2_MS; break;
  }

  if (now - lastToneTick >= phaseDur) {
    lastToneTick = now;
    sirenPhase   = (sirenPhase + 1) % 3;

    switch (sirenPhase) {
      case 0: buzzerTone(TONE_HIGH); break;   // start of next cycle
      case 1: buzzerTone(TONE_LOW);  break;   // drop to low
      case 2: buzzerOff();           break;   // silence gap
    }
  }
}

// ============================================================================
//  checkButton()
// ============================================================================
void checkButton() {
  unsigned long now     = millis();
  bool          reading = digitalRead(PIN_BUTTON);

  if (now - lastButtonCheck < DEBOUNCE_MS) return;

  if (reading == LOW && buttonLastState == HIGH) {
    lastButtonCheck = now;
    sys.simMode = !sys.simMode;

    logToSerial(sys.simMode
      ? F("SIM MODE ON  — relay blocked, fire values injected.")
      : F("SIM MODE OFF — returning to real sensors."));

    if (!sys.simMode) {
      buzzerOff();
      setRelay(false);
      lcd.clear();
      sirenPhase = 0;
    }
  }
  buttonLastState = reading;

  // ── Serial command listener (type WIFI + Enter to reconfigure) ───────────
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toUpperCase();
    if (cmd == "WIFI") {
      Serial.println(F("\n[CMD] Reconfiguring WiFi..."));
      scanAndConfigureWifi();
    } else if (cmd == "FORGET") {
      prefs.begin(NVS_NAMESPACE, false);
      prefs.clear();
      prefs.end();
      Serial.println(F("[NVS] Saved WiFi credentials erased. Restart to reconfigure."));
    } else if (cmd == "IP") {
      Serial.print(F("[WiFi] IP: "));
      Serial.println(sys.wifiConnected ? sys.localIP : F("Not connected"));
    }
  }
}

// ============================================================================
//  sendSensorData()  —  JSON line for web dashboard Serial bridge
// ============================================================================
void sendSensorData() {
  Serial.print(F("{\"temperature\":"));
  if (isnan(sys.sensors.temperature)) Serial.print(F("null"));
  else Serial.print(sys.sensors.temperature, 1);
  Serial.print(F(",\"smoke\":"));      Serial.print(sys.sensors.smokeRaw);
  Serial.print(F(",\"flame\":"));      Serial.print(sys.sensors.flameDetected ? F("true") : F("false"));
  Serial.print(F(",\"level\":"));      Serial.print(sys.alarmLevel);
  Serial.print(F(",\"pts\":"));        Serial.print(sys.fusion.points);
  Serial.print(F(",\"sim\":"));        Serial.print(sys.simMode ? F("true") : F("false"));
  Serial.print(F(",\"uptime\":\""));   Serial.print(uptimeStr());
  Serial.print(F("\",\"wifi\":"));     Serial.print(sys.wifiConnected ? F("true") : F("false"));
  Serial.print(F(",\"ip\":\""));       Serial.print(sys.localIP);
  Serial.println(F("\"}"));
}

// ============================================================================
//  HELPERS
// ============================================================================

void setLEDs(uint8_t level) {
  digitalWrite(PIN_LED_GREEN,  (level < 2)  ? HIGH : LOW);
  digitalWrite(PIN_LED_YELLOW, (level == 1) ? HIGH : LOW);
  if (level < 2) digitalWrite(PIN_LED_RED, LOW);
}

void setRelay(bool active) {
  // Active LOW: LOW = coil ON = fan running
  digitalWrite(PIN_RELAY, active ? LOW : HIGH);
  sys.relayActive = active;
}

// ESP32 core v3.x: ledcWriteTone(pin, freq) sets frequency + 50% duty.
// ledcWriteTone(pin, 0) stops output cleanly.
void buzzerTone(uint32_t freq) {
  ledcWriteTone(PIN_BUZZER, freq);
}

void buzzerOff() {
  ledcWriteTone(PIN_BUZZER, 0);
}

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
// ============================================================================
//  END OF FILE
// ============================================================================
