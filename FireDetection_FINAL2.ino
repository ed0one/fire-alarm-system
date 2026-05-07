// ============================================================================
//  INTELLIGENT FIRE DETECTION SYSTEM — ESP32 + Blynk IoT
//  Platform  : ESP32 (38-pin DevKit or WROOM-32)
//  Version   : 5.0.0
// ----------------------------------------------------------------------------
//  WHAT'S NEW in v5.0 (Blynk IoT integration):
//    - Blynk IoT push notifications on phone when fire/smoke detected
//    - fire_alert  event: triggers when alarm reaches DANGER (level 2)
//    - smoke_warning event: triggers when alarm reaches WARNING (level 1)
//    - Virtual pins for Blynk dashboard live widgets:
//        V0 = Temperature °C   (Gauge: 0–80)
//        V1 = Smoke ADC        (Gauge: 0–4095)
//        V2 = Flame 0/1        (LED widget)
//        V3 = Alarm level 0/1/2 (Value display)
//    - Uses Blynk.config() + Blynk.connect() so our custom NVS WiFi setup
//      remains intact — Blynk piggy-backs on the existing connection.
//    - blynk field added to JSON → web dashboard shows Blynk status chip.
//
//  SETUP STEPS (Blynk):
//    1. Install "Blynk" library via Arduino Library Manager (by Volodymyr Shymanskyy).
//    2. Create a free account at blynk.cloud.
//    3. New Project → ESP32 → Copy your Auth Token below.
//    4. Replace BLYNK_TEMPLATE_ID, BLYNK_TEMPLATE_NAME, BLYNK_AUTH_TOKEN below.
//    5. In Blynk console, create two Events:
//         • Event code: fire_alert    — enable push notification
//         • Event code: smoke_warning — enable push notification
//    6. Add widgets to Blynk dashboard: Gauge V0 (Temp), Gauge V1 (Smoke),
//       LED V2 (Flame), Value V3 (Level).
//    7. Flash to ESP32. On first boot, configure WiFi via Serial Monitor.
// ============================================================================

// ── Blynk credentials (replace with your own from blynk.cloud) ───────────────
// These MUST be defined BEFORE the Blynk include.
#define BLYNK_TEMPLATE_ID "TMPL4dmW8pOjR"
#define BLYNK_TEMPLATE_NAME "notify fire alarm"
#define BLYNK_AUTH_TOKEN "4tXML9n4uCwtbIGCPw20kBQJt7WJDXgk"  // 32-char token from blynk.cloud

// ── Libraries ────────────────────────────────────────────────────────────────
#include <Wire.h>
#include <LiquidCrystal_I2C.h>   // Frank de Brabander
#include <DHT.h>                  // Adafruit DHT sensor library
#include <WiFi.h>                 // ESP32 built-in
#include <Preferences.h>          // ESP32 NVS key-value flash storage
#include <BlynkSimpleEsp32.h>     // Blynk IoT (install via Library Manager)

// ── ESP32 Pin Map ─────────────────────────────────────────────────────────────
// IMPORTANT: GPIO 6–11 are reserved for SPI flash — never use them.
#define PIN_DHT         4    // DHT11 data
#define PIN_SMOKE       34   // MQ-2 analog (ADC1_CH6 — input only)
#define PIN_FLAME       13   // IR flame sensor — Active LOW
#define PIN_BUTTON      15   // Mode toggle — Active LOW, INPUT_PULLUP
#define PIN_BUZZER      25   // Passive buzzer — LEDC PWM
#define PIN_RELAY       16   // Relay / fan — Active LOW
#define PIN_LED_GREEN   27   // Green  LED (Safe + Warning)
#define PIN_LED_YELLOW  18   // Yellow LED (Warning only)
#define PIN_LED_RED     19   // Red    LED (Danger — blinks)

// I2C default on ESP32: SDA = GPIO 21, SCL = GPIO 22

// ── Sensor thresholds ─────────────────────────────────────────────────────────
#define DHT_TYPE         DHT11
#define TEMP_THRESHOLD   45.0f  // °C
#define SMOKE_THRESHOLD  500    // ADC 0–4095 (ESP32 12-bit)

// ── Timing constants (ms) ─────────────────────────────────────────────────────
#define INTERVAL_SENSOR    2000UL
#define INTERVAL_SERIAL   15000UL
#define INTERVAL_BLINK      500UL
#define INTERVAL_JSON      2000UL
#define INTERVAL_BLYNK     5000UL  // how often to push data to Blynk virtual pins
#define DEBOUNCE_MS          50UL
#define WIFI_TIMEOUT_MS   15000UL

// ── Buzzer 3-phase siren ──────────────────────────────────────────────────────
#define TONE_HIGH        1800
#define TONE_LOW          600
#define SIREN_PHASE_0_MS  400UL
#define SIREN_PHASE_1_MS  300UL
#define SIREN_PHASE_2_MS  300UL

// ── LEDC (buzzer PWM) ─────────────────────────────────────────────────────────
#define LEDC_CHANNEL     0
#define LEDC_RESOLUTION  8

// ── LCD ───────────────────────────────────────────────────────────────────────
#define LCD_ADDR  0x3F
#define LCD_COLS  16
#define LCD_ROWS  2

// ── NVS namespace ─────────────────────────────────────────────────────────────
#define NVS_NAMESPACE  "firedet"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "pass"

// ── Blynk virtual pins ────────────────────────────────────────────────────────
#define VPIN_TEMP    V0  // Gauge 0–80 °C
#define VPIN_SMOKE   V1  // Gauge 0–4095 ADC
#define VPIN_FLAME   V2  // LED widget (0 or 1)
#define VPIN_LEVEL   V3  // Value display (0 / 1 / 2)

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
  float temperature;
  float humidity;
  int   smokeRaw;
  bool  flameDetected;
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
  bool         blynkConnected;
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
static unsigned long lastBlynkTick   = 0;

// ── Siren / blink state ───────────────────────────────────────────────────────
static uint8_t sirenPhase     = 0;
static bool    lcdBlinkState  = false;
static bool    buttonLastState = HIGH;

// ── Blynk alarm transition tracking ──────────────────────────────────────────
static uint8_t blynkLastLevel = 255;  // sentinel — force first check

// ── Forward declarations ──────────────────────────────────────────────────────
void     readSensors();
void     processFusion();
void     updateDisplay();
void     handleAlarms();
void     checkButton();
void     setupWifi();
void     tryConnectWifi(const String& ssid, const String& pass);
void     scanAndConfigureWifi();
void     connectBlynk();
void     pushBlynkData();
void     sendBlynkEvent(uint8_t level);
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
  Serial.println(F("\n=== Fire Detection System v5.0.0 — ESP32 + Blynk ==="));

  pinMode(PIN_RELAY,      OUTPUT);
  pinMode(PIN_LED_GREEN,  OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED,    OUTPUT);
  pinMode(PIN_FLAME,      INPUT);
  pinMode(PIN_BUTTON,     INPUT_PULLUP);
  pinMode(PIN_SMOKE,      INPUT);

  ledcAttach(PIN_BUZZER, 1000, LEDC_RESOLUTION);
  buzzerOff();

  setRelay(false);
  setLEDs(0);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Fire Det v5.0  "));
  lcd.setCursor(0, 1); lcd.print(F("ESP32 + Blynk  "));

  dht.begin();

  memset(&sys, 0, sizeof(sys));
  sys.sensors.temperature = NAN;
  sys.sensors.humidity    = NAN;
  sys.wifiConnected       = false;
  sys.blynkConnected      = false;
  sys.localIP             = "Not connected";
  sys.wifiSSID            = "";

  // WiFi first, then Blynk piggy-backs on the connection
  setupWifi();

  delay(300);
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

  // Keep Blynk alive (non-blocking)
  if (sys.wifiConnected) {
    Blynk.run();
    sys.blynkConnected = Blynk.connected();
  }

  // Push sensor data to Blynk virtual pins every 5 s
  if (sys.wifiConnected && sys.blynkConnected && now - lastBlynkTick >= INTERVAL_BLYNK) {
    lastBlynkTick = now;
    pushBlynkData();
  }

  // JSON for web dashboard bridge every 2 s
  if (now - lastJsonTick >= INTERVAL_JSON) {
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
    msg += F(" | Smoke:"); msg += sys.sensors.smokeRaw;
    msg += F(" | Flame:"); msg += sys.sensors.flameDetected ? F("YES") : F("NO");
    msg += F(" | WiFi:");  msg += sys.wifiConnected ? sys.wifiSSID : String(F("OFF"));
    msg += F(" | Blynk:"); msg += sys.blynkConnected ? F("ON") : F("OFF");
    logToSerial(msg);
  }
}

// ============================================================================
//  setupWifi()
// ============================================================================
void setupWifi() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("WiFi Setup...  "));

  prefs.begin(NVS_NAMESPACE, true);
  String savedSSID = prefs.getString(NVS_KEY_SSID, "");
  String savedPass = prefs.getString(NVS_KEY_PASS, "");
  prefs.end();

  if (savedSSID.length() > 0) {
    Serial.print(F("[WiFi] Saved network: "));
    Serial.println(savedSSID);
    lcd.setCursor(0, 1);
    lcd.print(savedSSID.substring(0, 16));
    tryConnectWifi(savedSSID, savedPass);
    if (sys.wifiConnected) return;
    Serial.println(F("[WiFi] Saved credentials failed. Scanning…"));
  } else {
    Serial.println(F("[WiFi] No saved credentials. Scanning…"));
  }

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
  lcd.setCursor(0, 1); lcd.print(ssid.substring(0, 16));

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
  delay(1500);

  // Blynk connects after WiFi is up
  connectBlynk();
}

// ============================================================================
//  connectBlynk()
// ----------------------------------------------------------------------------
//  Uses Blynk.config() + Blynk.connect() so WiFi management stays in our
//  hands. Does NOT call Blynk.begin() to avoid it taking over WiFi.
// ============================================================================
void connectBlynk() {
  Serial.print(F("[Blynk] Connecting to blynk.cloud…"));
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Blynk connect.."));

  Blynk.config(BLYNK_AUTH_TOKEN);

  // 6-second non-blocking connect attempt
  sys.blynkConnected = Blynk.connect(6000);

  if (sys.blynkConnected) {
    Serial.println(F(" OK"));
    lcd.setCursor(0, 1); lcd.print(F("Blynk: OK      "));
    delay(1200);
  } else {
    Serial.println(F(" FAILED"));
    Serial.println(F("[Blynk] Will retry in background via Blynk.run()."));
    lcd.setCursor(0, 1); lcd.print(F("Blynk: offline "));
    delay(1200);
  }
}

// ============================================================================
//  scanAndConfigureWifi()
// ============================================================================
void scanAndConfigureWifi() {
  Serial.println(F("\n╔═══════════════════════════════════╗"));
  Serial.println(F("║     WiFi Network Configuration    ║"));
  Serial.println(F("╚═══════════════════════════════════╝"));
  Serial.println(F("Open Serial Monitor at 115200 baud."));
  Serial.println(F("Scanning for networks…\n"));

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Scanning WiFi.."));

  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println(F("[WiFi] No networks found. Skipping."));
    lcd.setCursor(0, 1); lcd.print(F("No networks!   "));
    delay(2000);
    return;
  }

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

  Serial.print(F("\nEnter network number (1–"));
  Serial.print(n);
  Serial.println(F(") or 0 to skip: "));

  String inputNum = "";
  unsigned long waitStart = millis();
  while (millis() - waitStart < 60000UL) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (inputNum.length() > 0) break;
      } else {
        inputNum += c;
        Serial.print(c);
      }
    }
  }
  Serial.println();

  int choice = inputNum.toInt();
  if (choice < 1 || choice > n) {
    Serial.println(F("[WiFi] Skipping."));
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(F("WiFi Skipped   "));
    delay(1500);
    return;
  }

  String chosenSSID = WiFi.SSID(choice - 1);
  bool   isOpen     = (WiFi.encryptionType(choice - 1) == WIFI_AUTH_OPEN);

  Serial.print(F("Selected: "));
  Serial.println(chosenSSID);

  String password = "";
  if (!isOpen) {
    Serial.print(F("Enter password for \""));
    Serial.print(chosenSSID);
    Serial.println(F("\" (then press Enter): "));

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(F("Enter Password "));
    lcd.setCursor(0, 1); lcd.print(F("in Serial Mon. "));

    waitStart = millis();
    while (millis() - waitStart < 120000UL) {
      if (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
          if (password.length() > 0) break;
        } else {
          password += c;
          Serial.print('*');
        }
      }
    }
    Serial.println();
  }

  tryConnectWifi(chosenSSID, password);

  if (sys.wifiConnected) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_SSID, chosenSSID);
    prefs.putString(NVS_KEY_PASS, password);
    prefs.end();
    Serial.println(F("[NVS] Credentials saved. Auto-connect on next boot."));
  } else {
    Serial.println(F("[WiFi] Failed. Running without WiFi."));
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
    sys.sensors.smokeRaw      = 600;
    sys.sensors.flameDetected = true;
    return;
  }

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    logToSerial(F("WARN: DHT11 read failed — keeping last valid reading."));
  } else {
    sys.sensors.temperature = t;
    sys.sensors.humidity    = h;
  }

  sys.sensors.smokeRaw      = analogRead(PIN_SMOKE);
  sys.sensors.flameDetected = (digitalRead(PIN_FLAME) == LOW);
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

  // ── Send Blynk event on alarm level change (real mode only) ────────────────
  if (sys.alarmLevel != blynkLastLevel && sys.blynkConnected && !sys.simMode) {
    sendBlynkEvent(sys.alarmLevel);
  }
  blynkLastLevel = sys.alarmLevel;
}

// ============================================================================
//  sendBlynkEvent()
// ----------------------------------------------------------------------------
//  Triggers push notification on the phone via a Blynk Event.
//  Events must be created in the Blynk console with these exact codes.
// ============================================================================
void sendBlynkEvent(uint8_t level) {
  const SensorData& s = sys.sensors;

  if (level == 2) {
    // DANGER: full alarm — fire_alert event
    String desc = F("DANGER: Fire! T=");
    desc += isnan(s.temperature) ? String(F("ERR")) : String(s.temperature, 1) + String(F("C"));
    desc += F(" Smoke="); desc += s.smokeRaw;
    desc += F(" Flame="); desc += s.flameDetected ? F("YES") : F("NO");
    Blynk.logEvent("fire_alert", desc);
    logToSerial(F("[Blynk] fire_alert event sent."));

  } else if (level == 1) {
    // WARNING: threshold crossed
    String desc = F("WARNING: Threshold exceeded. Smoke=");
    desc += s.smokeRaw;
    desc += F(" T=");
    desc += isnan(s.temperature) ? String(F("ERR")) : String(s.temperature, 1) + String(F("C"));
    Blynk.logEvent("smoke_warning", desc);
    logToSerial(F("[Blynk] smoke_warning event sent."));
  }
  // level == 0 (back to safe) — no notification needed
}

// ============================================================================
//  pushBlynkData()  — update virtual pins in Blynk dashboard
// ============================================================================
void pushBlynkData() {
  if (!isnan(sys.sensors.temperature))
    Blynk.virtualWrite(VPIN_TEMP,  sys.sensors.temperature);
  Blynk.virtualWrite(VPIN_SMOKE, sys.sensors.smokeRaw);
  Blynk.virtualWrite(VPIN_FLAME, sys.sensors.flameDetected ? 1 : 0);
  Blynk.virtualWrite(VPIN_LEVEL, sys.alarmLevel);
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
//  handleAlarms()  — 3-phase siren + blink
// ============================================================================
void handleAlarms() {
  unsigned long now = millis();

  if (sys.alarmLevel < 2) {
    buzzerOff();
    setRelay(false);
    sirenPhase = 0;
    return;
  }

  setRelay(!sys.simMode);

  if (now - lastBlinkTick >= INTERVAL_BLINK) {
    lastBlinkTick = now;
    lcdBlinkState = !lcdBlinkState;
    digitalWrite(PIN_LED_RED, lcdBlinkState ? HIGH : LOW);
    lcd.setCursor(0, 0);
    lcd.print(lcdBlinkState ? F("*** FIRE ***    ") : F("                "));
    printLcdRow1();
  }

  unsigned long phaseDur;
  switch (sirenPhase) {
    case 0:  phaseDur = SIREN_PHASE_0_MS; break;
    case 1:  phaseDur = SIREN_PHASE_1_MS; break;
    default: phaseDur = SIREN_PHASE_2_MS; break;
  }

  if (now - lastToneTick >= phaseDur) {
    lastToneTick = now;
    sirenPhase   = (sirenPhase + 1) % 3;
    switch (sirenPhase) {
      case 0: buzzerTone(TONE_HIGH); break;
      case 1: buzzerTone(TONE_LOW);  break;
      case 2: buzzerOff();           break;
    }
  }
}

// ============================================================================
//  checkButton() + Serial commands
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

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toUpperCase();
    if (cmd == "WIFI") {
      Serial.println(F("\n[CMD] Reconfiguring WiFi…"));
      scanAndConfigureWifi();
    } else if (cmd == "FORGET") {
      prefs.begin(NVS_NAMESPACE, false);
      prefs.clear();
      prefs.end();
      Serial.println(F("[NVS] WiFi credentials erased. Restart to reconfigure."));
    } else if (cmd == "IP") {
      Serial.print(F("[WiFi] IP: "));
      Serial.println(sys.wifiConnected ? sys.localIP : F("Not connected"));
    } else if (cmd == "BLYNK") {
      Serial.print(F("[Blynk] Connected: "));
      Serial.println(sys.blynkConnected ? F("YES") : F("NO"));
    }
  }
}

// ============================================================================
//  sendSensorData()  — JSON line for web dashboard Serial bridge
// ============================================================================
void sendSensorData() {
  Serial.print(F("{\"temperature\":"));
  if (isnan(sys.sensors.temperature)) Serial.print(F("null"));
  else Serial.print(sys.sensors.temperature, 1);
  Serial.print(F(",\"smoke\":"));       Serial.print(sys.sensors.smokeRaw);
  Serial.print(F(",\"flame\":"));       Serial.print(sys.sensors.flameDetected ? F("true") : F("false"));
  Serial.print(F(",\"level\":"));       Serial.print(sys.alarmLevel);
  Serial.print(F(",\"pts\":"));         Serial.print(sys.fusion.points);
  Serial.print(F(",\"sim\":"));         Serial.print(sys.simMode ? F("true") : F("false"));
  Serial.print(F(",\"uptime\":\""));    Serial.print(uptimeStr());
  Serial.print(F("\",\"wifi\":"));      Serial.print(sys.wifiConnected ? F("true") : F("false"));
  Serial.print(F(",\"ip\":\""));        Serial.print(sys.localIP);
  Serial.print(F("\",\"blynk\":"));     Serial.print(sys.blynkConnected ? F("true") : F("false"));
  Serial.println(F("}"));
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
  digitalWrite(PIN_RELAY, active ? LOW : HIGH);  // Active LOW
  sys.relayActive = active;
}

void buzzerTone(uint32_t freq) { ledcWriteTone(PIN_BUZZER, freq); }
void buzzerOff()               { ledcWriteTone(PIN_BUZZER, 0);    }

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
