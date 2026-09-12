// =====================================================================
// SPUMCS v2 - Smart Per-device Usage & Monitoring Control System
// =====================================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// --------------------------- LCD ------------------------------------
// If the screen stays blank, your module's I2C address may be 0x3F
// instead of 0x27 -- both are common. Also verify the chip type is
// "PCF8574" and not "MCP23008" or "MCP23017" -- those need a different library.
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --------------------------- Pin map ---------------------------------
const int LATCH = 12, CLOCK = 13, DATA = 11;

const int P_BRK1 = 2, P_BRK2 = 3;
const int P_RESET = A2;
const int P_POT_R1 = A0;   // simulated Room 1 aggregate current sensor
const int P_POT_R2 = A1;   // simulated Room 2 aggregate current sensor
const int P_RELAY_CTRL = A3; // drives NPN base -> relay coil -> AC contact

// --------------------------- Device model -----------------------------
struct Device {
  const char* name;
  uint8_t switchPin;
  uint8_t room;          // 1 or 2
  float watts;           // nominal wattage when ON
  uint8_t ledBit;        // which bit this device owns in the 16-bit LED word
  bool isShedTarget;     // true only for the relay-controlled device
  bool cutByRelay;       // true while the relay has forcibly shed it
  float cumWh;           // cumulative energy, Wh (this is what EEPROM stores)
};

// Bit layout in the 16-bit word sent to the two chained 74HC595s:
//   bit0 BRK1   bit1 BRK2
//   bit2 r1_fan bit3 r1_lights bit4 r1_tv
//   bit5 r2_air bit6 r2_lights bit7 r2_exh   bit8 r2_micro
Device devices[7] = {
  { "R1 Fan",     4, 1,   75.0, 2, false, false, 0.0 },
  { "R1 Lights",  5, 1,   40.0, 3, false, false, 0.0 },
  { "R1 TV",      6, 1,  120.0, 4, false, false, 0.0 },
  { "R2 AC",      7, 2, 1400.0, 5, true,  false, 0.0 },   // <-- shed target
  { "R2 Lights",  8, 2,   40.0, 6, false, false, 0.0 },
  { "R2 Exhaust", 9, 2,   60.0, 7, false, false, 0.0 },
  { "R2 Microwave",10,2,  800.0, 8, false, false, 0.0 }
};
const int NUM_DEVICES = 7;

// --------------------------- Timing -----------------------------------
const unsigned long INTERVAL_MS   = 5000UL;   // energy accumulation tick
const unsigned long EEPROM_MS     = 60000UL;  // how often we persist to EEPROM
// {The EEPROM does not persist across a Tinkercad simulation restart, so
//  it will read back as "no valid data" every time you press Start again.
//  The read/write logic itself is correct and would work on real hardware.}
const unsigned long LCD_PAGE_MS   = 2500UL;   // how long each LCD page shows
const unsigned long LCD_UPDATE_MS = 300UL;    // how often we check for changes to redraw
// NOTE: Tinkercad's simulated clock runs slower than real time, so these
// values will feel longer while watching the sim than the numbers suggest.
const unsigned long DEBOUNCE_MS   = 300UL;    // reset-button debounce
const uint8_t NUM_LCD_PAGES = 2;              // 0: total kWh, 1: house/room power

unsigned long lastInterval = 0;
unsigned long lastEepromSave = 0;
unsigned long lastLcdSwitch = 0;
unsigned long lastLcdUpdate = 0;
unsigned long lastResetPress = 0;
uint8_t lcdPage = 0;

// --- LCD flicker fix: only re-send a line if its text actually changed ---
// Uses Arduino's built-in String class -- no extra headers/libraries needed.
String lcdLine0 = "";
String lcdLine1 = "";

// --- Overload alert: force-shows on the LCD the instant it happens ---
bool alertActive = false;
unsigned long alertUntil = 0;
const unsigned long ALERT_HOLD_MS = 4000UL; // how long an alert stays on screen
String alertLine0 = "";
String alertLine1 = "";

// --- what was actually loaded from EEPROM at boot (for the "prev stored" page) ---
float eepromLoadedKwh = 0.0;

// --------------------------- Overload thresholds ------------------------
const float HOUSE_SHED_W    = 2000.0; // shed the AC above this total house wattage
const float HOUSE_RESTORE_W = 1600.0; // restore it once load drops below this
// The gap between shed/restore is hysteresis: it stops the relay from
// rapidly clicking on/off when the load sits right at one threshold.
// NOTE: because the AC itself is 1400W, shedding/restoring it can swing
// house_power by more than this gap all on its own -- so we ALSO enforce
// a minimum cooldown between transitions below, to stop rapid chattering.
const unsigned long RELAY_COOLDOWN_MS = 5000UL; // min time between shed/restore flips
unsigned long lastRelayChange = 0;

// --------------------------- NILM (appliance guessing) -----------------
const float NILM_DELTA_MIN = 30.0;   // ignore tiny noise/jitter
const float NILM_TOLERANCE = 0.15;   // match within +/-15% of a known wattage
float lastPotWatts[2] = { 0.0, 0.0 }; // previous reading, per room (0=R1,1=R2)

// --------------------------- EEPROM layout ------------------------------
const int EEPROM_MAGIC_ADDR = 0;
const int EEPROM_DATA_ADDR  = 2;
const uint16_t EEPROM_MAGIC = 0xBEEF; // marks "valid data has been saved here"

// =====================================================================
inline bool readOn(int pin) { return digitalRead(pin) == LOW; }

// Chained 74HC595 write
void srWrite(unsigned int bits) {
  digitalWrite(LATCH, LOW);
  shiftOut(DATA, CLOCK, MSBFIRST, (bits >> 8) & 0xFF);
  delay(1);
  shiftOut(DATA, CLOCK, MSBFIRST, bits & 0xFF);
  digitalWrite(LATCH, HIGH);
}

// Pads a String out to 16 chars so old text is fully overwritten on the LCD.
String pad16(String s) {
  while (s.length() < 16) s += ' ';
  if (s.length() > 16) s = s.substring(0, 16);
  return s;
}

// Only sends a line to the LCD if its text actually changed since last time.
// This is what kills the flicker: no blind clear()+reprint every cycle.
void lcdShow(String line0, String line1) {
  line0 = pad16(line0);
  line1 = pad16(line1);
  if (line0 != lcdLine0) {
    lcd.setCursor(0, 0);
    lcd.print(line0);
    lcdLine0 = line0;
  }
  if (line1 != lcdLine1) {
    lcd.setCursor(0, 1);
    lcd.print(line1);
    lcdLine1 = line1;
  }
}

// Forces the LCD to immediately show an overload event, overriding whatever
// page was on screen, and holds it for ALERT_HOLD_MS so a shed/restore can
// never silently pass by unseen.
void triggerAlert(String l0, String l1) {
  alertLine0 = l0;
  alertLine1 = l1;
  alertActive = true;
  alertUntil = millis() + ALERT_HOLD_MS;
}

// ---- EEPROM helpers ----
void saveToEeprom() {
  float buf[NUM_DEVICES];
  for (int i = 0; i < NUM_DEVICES; i++) buf[i] = devices[i].cumWh;
  EEPROM.put(EEPROM_DATA_ADDR, buf);
  EEPROM.put(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
}

void loadFromEeprom() {
  uint16_t magic;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  if (magic == EEPROM_MAGIC) {
    float buf[NUM_DEVICES];
    EEPROM.get(EEPROM_DATA_ADDR, buf);
    float totalWh = 0.0;
    for (int i = 0; i < NUM_DEVICES; i++) {
      devices[i].cumWh = buf[i];
      totalWh += buf[i];
    }
    eepromLoadedKwh = totalWh / 1000.0;
    Serial.println(F("EEPROM: restored cumulative energy."));
  } else {
    eepromLoadedKwh = 0.0;
    Serial.println(F("EEPROM: no valid data yet, starting from 0."));
  }
}

void resetAll() {
  for (int i = 0; i < NUM_DEVICES; i++) devices[i].cumWh = 0.0;
  saveToEeprom();
  Serial.println(F("RESET: all cumulative energy cleared (and saved)."));
}

// ---- NILM: guess which device caused a sudden power jump ----
void nilmCheck(int roomIndex, float newWatts) {
  float delta = newWatts - lastPotWatts[roomIndex];
  lastPotWatts[roomIndex] = newWatts;
  if (fabs(delta) < NILM_DELTA_MIN) return; // just noise, ignore

  int room = roomIndex + 1;
  const char* bestName = nullptr;
  float bestErr = 999999.0;

  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].room != room) continue;
    float err = fabs(fabs(delta) - devices[i].watts) / devices[i].watts;
    if (err < NILM_TOLERANCE && err < bestErr) {
      bestErr = err;
      bestName = devices[i].name;
    }
  }

  Serial.print(F("[NILM] Room ")); Serial.print(room);
  Serial.print(delta > 0 ? F(" jump +") : F(" drop "));
  Serial.print(delta, 1); Serial.print(F("W -> "));
  if (bestName) { Serial.print(F("likely ")); Serial.println(bestName); }
  else Serial.println(F("no confident match"));
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(LATCH, OUTPUT); pinMode(CLOCK, OUTPUT); pinMode(DATA, OUTPUT);
  pinMode(P_BRK1, INPUT_PULLUP);
  pinMode(P_BRK2, INPUT_PULLUP);
  for (int i = 0; i < NUM_DEVICES; i++) pinMode(devices[i].switchPin, INPUT_PULLUP);
  pinMode(P_RESET, INPUT_PULLUP);
  pinMode(P_RELAY_CTRL, OUTPUT);
  digitalWrite(P_RELAY_CTRL, LOW); // relay OFF (AC not being force-fed power) at boot

  srWrite(0);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(F("SPUMCS v2 boot"));

  loadFromEeprom();
  triggerAlert("Prev stored:", String(eepromLoadedKwh, 4) + " kWh");

  unsigned long now = millis();
  lastInterval = now;
  lastEepromSave = now;
  lastLcdSwitch = now;

  Serial.println(F("SPUMCS v2 ready."));
}

// =====================================================================
void loop() {
  unsigned long now = millis();

  // ---------- 2. Non-blocking reset debounce ----------
  if (readOn(P_RESET) && (now - lastResetPress > DEBOUNCE_MS)) {
    resetAll();
    lastResetPress = now;
  }

  // ---------- read breakers ----------
  bool brk1 = readOn(P_BRK1);
  bool brk2 = readOn(P_BRK2);

  // ---------- read every device switch & compute instantaneous power ----------
  float power[NUM_DEVICES];
  float room1_power = 0.0, room2_power = 0.0;

  for (int i = 0; i < NUM_DEVICES; i++) {
    bool brk = (devices[i].room == 1) ? brk1 : brk2;
    bool swOn = readOn(devices[i].switchPin);
    bool on = brk && swOn && !devices[i].cutByRelay;
    power[i] = on ? devices[i].watts : 0.0;
    if (devices[i].room == 1) room1_power += power[i];
    else room2_power += power[i];
  }
  float house_power = room1_power + room2_power;

  // ---------- 4. Overload protection: relay-based load shedding ----------
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (!devices[i].isShedTarget) continue;
    bool cooldownOver = (now - lastRelayChange) >= RELAY_COOLDOWN_MS;
    if (!devices[i].cutByRelay && house_power > HOUSE_SHED_W && cooldownOver) {
      devices[i].cutByRelay = true;
      lastRelayChange = now;
      Serial.println(F("[OVERLOAD] House power too high -> shedding R2 AC."));
      triggerAlert("OVERLOAD!", "AC SHED");
    } else if (devices[i].cutByRelay && house_power < HOUSE_RESTORE_W && cooldownOver) {
      devices[i].cutByRelay = false;
      lastRelayChange = now;
      Serial.println(F("[OVERLOAD] Load back to normal -> restoring R2 AC."));
      triggerAlert("LOAD NORMAL", "AC RESTORED");
    }
    digitalWrite(P_RELAY_CTRL, devices[i].cutByRelay ? LOW : HIGH);
  }

  // ---------- update the 9 status LEDs ----------
  unsigned int bits = 0;
  if (brk1) bits |= (1 << 0);
  if (brk2) bits |= (1 << 1);
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (power[i] > 0) bits |= (1UL << devices[i].ledBit);
  }
  srWrite(bits);

  // ---------- 6. Potentiometer "current sensors" + 5. NILM check ----------
  float potR1Watts = map(analogRead(P_POT_R1), 0, 1023, 0, 500);
  float potR2Watts = map(analogRead(P_POT_R2), 0, 1023, 0, 2400);
  nilmCheck(0, potR1Watts);
  nilmCheck(1, potR2Watts);

  // ---------- 1 & 3. Interval energy accumulation + EEPROM persistence ----------
  if (now - lastInterval >= INTERVAL_MS) {
    lastInterval += INTERVAL_MS;
    float hrs = (float)INTERVAL_MS / 3600000.0;

    for (int i = 0; i < NUM_DEVICES; i++) {
      devices[i].cumWh += power[i] * hrs;
    }

    Serial.println(F("---- interval report ----"));
    Serial.print(F("House power (W): ")); Serial.println(house_power, 1);
    Serial.print(F("House cumulative (kWh): "));
    float totalWh = 0;
    for (int i = 0; i < NUM_DEVICES; i++) totalWh += devices[i].cumWh;
    Serial.println(totalWh / 1000.0, 6);
  }

  if (now - lastEepromSave >= EEPROM_MS) {
    lastEepromSave = now;
    saveToEeprom();
    Serial.println(F("EEPROM: cumulative energy saved."));
  }

  // ---------- 7. LCD: cycle through info pages, flicker-free ----------
  if (now - lastLcdSwitch >= LCD_PAGE_MS) {
    lastLcdSwitch = now;
    lcdPage = (lcdPage + 1) % NUM_LCD_PAGES;
  }

  if (now - lastLcdUpdate >= LCD_UPDATE_MS) {
    lastLcdUpdate = now;

    // An active overload alert always wins -- it can't be missed by the
    // page cycle happening to be pointed somewhere else.
    if (alertActive) {
      if ((long)(now - alertUntil) < 0) {
        lcdShow(alertLine0, alertLine1);
      } else {
        alertActive = false;
      }
    }

    if (!alertActive) {
      float totalWh = 0;
      for (int i = 0; i < NUM_DEVICES; i++) totalWh += devices[i].cumWh;
      String l0 = "Total kWh:" + String(totalWh / 1000.0, 4); // always shown, top line
      String l1;
      if (lcdPage == 0) {
        l1 = "House: " + String((int)house_power) + "W";
      } else {
        l1 = "R1:" + String((int)room1_power) + " R2:" + String((int)room2_power) + "W";
      }
      lcdShow(l0, l1);
    }
  }

  delay(20);
}

