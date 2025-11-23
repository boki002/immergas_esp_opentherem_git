/*
  ESP32 + OpenTherm padlófűtés / radiátor szabályzó
  -------------------------------------------------

  A rendszer két külön fűtési kört irányít:
  - padlófűtés (35–42 °C tartomány)
  - radiátoros kör (45–65 °C tartomány)

  A két kör bemenete (IN1/IN2) külső kontaktokkal (NO/relé) vezérelhető.
  A kívánt előremenő hőmérsékleteket két potméter (ADC) állítja.

  MŰKÖDÉSI LOGIKA:
  ----------------
  • Ha valamelyik zóna aktív, a kazán CH üzem engedélyezésre kerül.
  • A célhőmérsékletet a zónák alapján választja:
      - csak padló aktív → padlóhőfok
      - csak radiátor aktív → radiátorhőfok
      - mindkettő aktív → a radiátor hőfok (zóna szelep zárja padló kört)
  
  • Radiátoros kör indításakor „lágy indítást” alkalmaz:
      3 percig csak minimális radiátor előremenőt kér (soft start).

  • HMV prioritás:
      A kazánból érkező OpenTherm válaszból kiolvassuk,
      hogy éppen melegvizet készít-e.
      Ha HMV aktív → CH parancsok nem kerülnek kiküldésre.
      (Teljes HMV elsőbbség fűtéssel szemben.)
      MHV 50C fokra állítva

  • OpenTherm kommunikáció 1 Hz ciklussal:
      - CH/HMV státusz beállítása
      - hőfokküldés (csak ha nem HMV)
      - OT válasz státusz kiértékelése (debug + hibafelügyelet)

  • OLED kijelző mutatja:
      - beállított hőfokokat
      - aktív zónát
      - kazán üzemmódot (OFF / CH / HMV)
      - küldött előremenő hőmérsékletet
      - radiátor soft start visszaszámlálást

  A kód biztosítja a zavartalan működést, a két kör közti prioritásokat,
  valamint a biztonságos HMV elsőbbséget a fűtéssel szemben.
*/




#include <Arduino.h>
#include <Wire.h>
#include <OpenTherm.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// PIN KIOSZTÁS
// OpenTherm
const int OT_IN_PIN  = 21;   // Boiler -> ESP32 (RX)
const int OT_OUT_PIN = 22;   // ESP32 -> Boiler (TX)
// OpenTherm objektum
OpenTherm ot(OT_IN_PIN, OT_OUT_PIN);


// Zónabemenetek (külső felhúzó ellenállással 3.3V-ra!)
const int PADLO_IN_PIN    = 35; // in1
const int RADIATOR_IN_PIN = 34; // in2

// Potméterek (ADC1)
const int PADLO_POT_PIN    = 32; // poti1
const int RADIATOR_POT_PIN = 25; // poti2

// I2C OLED (D096-12864-I2C-GV-WH, feltételezve SSD1306)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
#define SCREEN_ADDRESS 0x3C  // ha nem jó, próbáld 0x3D-vel

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);


// Beállított hőmérsékletek (potikból)
float setTempPadlo    = 35.0;
float setTempRadiator = 55.0;

// Tartományok (tetszés szerint módosítható)
const float MIN_PADLO_TEMP = 35.0;
const float MAX_PADLO_TEMP = 42.0;
const float MIN_RAD_TEMP   = 45.0;
const float MAX_RAD_TEMP   = 65.0;

// Radiátoros „lágy indítás” (3 perc minimum hőfok)
const unsigned long RADIATOR_TRANSITION_MS = 180000UL;  // 3 perc = 180 s
const float RADIATOR_TRANSITION_TEMP = MIN_RAD_TEMP;    // ezt küldjük átmenetben

// Állapot
bool padloActive    = false;
bool radiatorActive = false;
bool lastRadiatorActive = false;

bool radiatorTransition = false;
unsigned long radiatorTransitionStart = 0;

float commandedTemp = 0.0;
bool boilerOn       = false;

// álapot jelző villogás
bool blinkState = false;
unsigned long lastBlinkMillis = 0;
const unsigned long BLINK_PERIOD = 500;  // 0.5 másodperc




// OpenTherm ciklus időzítő (1 Hz)
unsigned long lastOtMillis = 0;
const unsigned long OT_PERIOD_MS = 1000UL;  // 1 s


bool dhwActive = false;  // HMV aktív-e a kazán szerint (OT-ből olvasva)

const float DHW_SETPOINT = 50.0;  // kívánt HMV hőfok (pl. 50 °C)



// ---- OpenTherm megszakításkezelő ----
void IRAM_ATTR handleInterrupt()
{
  ot.handleInterrupt();
}

// Segédfüggvény: float map
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ---- Potméter beolvasása 10k-s potival ----
// Több minta átlagolása zaj ellen
int readPot(int pin) {
  const int samples = 16;  // 16 mintát átlagolunk
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(300);   // minimális várakozás két mintavétel között
  }
  return sum / samples; // átlagolt ADC érték (0..4095)
}

// OLED frissítés
void updateDisplay()
{
  extern bool otOk;

  extern bool dhwActive;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("Padlo:    ");
  display.print(setTempPadlo, 1);
  display.print(" C");

  display.setCursor(0, 10);
  display.print("Radiator: ");
  display.print(setTempRadiator, 1);
  display.print(" C");

  display.setCursor(0, 22);
  display.print("Aktiv:    ");
  if (padloActive && !radiatorActive) {
    display.print("Padlo");
  } else if (!padloActive && radiatorActive) {
    display.print("Rad");
  } else if (padloActive && radiatorActive) {
    display.print("Mindketto");
  } else {
    display.print("Nincs");
  }

  display.setCursor(0, 34);
  display.print("Kazan:    ");
  if (dhwActive) {
    display.print("HMV");
  } else if (boilerOn) {
    display.print("CH ");
  } else {
    display.print("OFF");
  }


  display.setCursor(0, 46);
  display.print("Kuldo:    ");
  if (boilerOn) {
    display.print(commandedTemp, 1);
    display.print(" C");
  } else {
    display.print("--.- C");
  }

  // Radiator „soft start” visszaszámlálás
  if (radiatorTransition && radiatorActive) {
    unsigned long now = millis();
    unsigned long elapsed = now - radiatorTransitionStart;
    long remain = (RADIATOR_TRANSITION_MS > elapsed)
                  ? (RADIATOR_TRANSITION_MS - elapsed)
                  : 0;
    int sec = remain / 1000;
    int mm = sec / 60;
    int ss = sec % 60;

    display.setCursor(0, 56);
    display.print("Rad varakozas: ");
    if (remain > 0) {
      if (mm < 10) display.print('0');
      display.print(mm);
      display.print(":");
      if (ss < 10) display.print('0');
      display.print(ss);
    } else {
      display.print("VEGE");
    }
  }


  display.display();
}

void setup()
{
  Serial.begin(115200);
  delay(500);

  // Zóna bemenetek – csak INPUT, mert GPIO34/35-ben nincs belső pullup!
  pinMode(PADLO_IN_PIN, INPUT);
  pinMode(RADIATOR_IN_PIN, INPUT);

  // Potméterek
  pinMode(PADLO_POT_PIN, INPUT);
  pinMode(RADIATOR_POT_PIN, INPUT);

  // I2C: SDA=33, SCL=27
  Wire.begin(27, 33);

  // OLED inicializálás
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED init hiba!");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("ESP32 OpenTherm");
    display.setCursor(0, 10);
    display.println("Inditas...");
    display.display();
  }

  // OpenTherm inicializálás
  ot.begin(handleInterrupt);

  Serial.println("Rendszer indult.");
}

void loop()
{
  unsigned long now = millis();

  // OpenTherm válasz státusz kiolvasása
  OpenThermResponseStatus otStatus = ot.getLastResponseStatus();
  bool otOk = (otStatus == OpenThermResponseStatus::SUCCESS);


  // Villogó állapotjelző
  if (now - lastBlinkMillis >= BLINK_PERIOD) {
      lastBlinkMillis = now;
      blinkState = !blinkState;
  }



  // Zónák (LOW = aktív, feltételezve külső felhúzó 3.3V-ra)
  padloActive    = (digitalRead(PADLO_IN_PIN) == LOW);
  radiatorActive = (digitalRead(RADIATOR_IN_PIN) == LOW);

  // Radiator mód váltás figyelése (rising edge)
  // Radiátor-only mód meghatározása
  bool radiatorOnly = (radiatorActive && !padloActive);

  // Radiátor-only üzem START detektálása (rising edge)
  if (radiatorOnly && !lastRadiatorActive) {
      radiatorTransition = true;
      radiatorTransitionStart = now;
  }

  // Ha már nem radiátor-only → soft start álljon le
  if (!radiatorOnly) {
      radiatorTransition = false;
  }

  lastRadiatorActive = radiatorOnly;


  // Potik – átlagolt ADC olvasás 10k-s potira
  int padloRaw    = readPot(PADLO_POT_PIN);
  float padloNorm = 1.0f - (float)padloRaw / 4095.0f;
  setTempPadlo = mapFloat(padloNorm, 0.0, 1.0, MIN_PADLO_TEMP, MAX_PADLO_TEMP);

  int radiatorRaw    = readPot(RADIATOR_POT_PIN);
  float radiatorNorm = 1.0f - (float)radiatorRaw / 4095.0f;
  setTempRadiator = mapFloat(radiatorNorm, 0.0, 1.0, MIN_RAD_TEMP, MAX_RAD_TEMP);



  // Alapértelmezett kimenő hőfok logika
  boilerOn = false;
  float targetTemp = 0.0;

  if (padloActive || radiatorActive) {
    boilerOn = true;

    if (padloActive && !radiatorActive) {
      targetTemp = setTempPadlo;
    } else if (!padloActive && radiatorActive) {
      targetTemp = setTempRadiator;
    } else if (padloActive && radiatorActive) {
      // itt most a KISEBBIK megy ki (padlóvédelem)
      targetTemp = max(setTempPadlo, setTempRadiator);
    }
  }

  // Radiator „soft start” logika – ha folyamatban van, felülírja a targetTemp-et
  if (radiatorTransition && radiatorActive) {
    unsigned long elapsed = now - radiatorTransitionStart;
    if (elapsed < RADIATOR_TRANSITION_MS) {
      // Átmeneti szakasz: mindig a minimum radiátor hőfokot küldjük
      commandedTemp = RADIATOR_TRANSITION_TEMP;
    } else {
      // Letelt a 3 perc
      radiatorTransition = false;
      commandedTemp = targetTemp;
    }
  } else {
    commandedTemp = targetTemp;
  }

  // OpenTherm ciklus – külön időzítő (1 Hz)
  if (now - lastOtMillis >= OT_PERIOD_MS) {
    lastOtMillis = now;

    // CH csak akkor engedélyezett, ha van zóna és épp NEM HMV megy
    bool chEnable  = boilerOn && !dhwActive;
    // HMV mindig engedélyezve – a kazán belső logikája dönti el, mikor kell
    bool dhwEnable = true;

    // 1) Boiler státusz beállítása és HMV állapot kiolvasása
    unsigned long resp = ot.setBoilerStatus(chEnable, dhwEnable, false);
    OpenThermResponseStatus st = ot.getLastResponseStatus();

    if (st == OpenThermResponseStatus::SUCCESS) {
      dhwActive = ot.isHotWaterActive(resp);  // kazán szerint épp HMV-üzem megy-e
    } else {
      dhwActive = false; // ha nincs értelmes válasz, ne higgyük azt, hogy HMV megy
    }

    // 2) Ha NEM HMV megy és van fűtési igény → küldjük a CH előremenő hőfokot
    if (!dhwActive && boilerOn) {
      ot.setBoilerTemperature(commandedTemp);
    }

    // 3) HMV setpont mindig beállítva (pl. 50 °C)
    ot.setDHWSetpoint(DHW_SETPOINT);
  }



  // Debug
  Serial.print("PadloIn=");
  Serial.print(padloActive);
  Serial.print(" RadIn=");
  Serial.print(radiatorActive);
  Serial.print(" setPadlo=");
  Serial.print(setTempPadlo);
  Serial.print(" setRad=");
  Serial.print(setTempRadiator);
  Serial.print(" targetTemp=");
  Serial.print(targetTemp);
  Serial.print(" commanded=");
  Serial.print(commandedTemp);
  Serial.print("  padloRaw=");
  Serial.print(padloRaw);
  Serial.print("  radRaw=");
  Serial.print(radiatorRaw);
  Serial.print("  radTrans=");
  Serial.println(radiatorTransition);

  // -------- OpenTherm debug kiírás --------
  OpenThermResponseStatus st = ot.getLastResponseStatus();
  Serial.print("OT status: ");

  if (st == OpenThermResponseStatus::SUCCESS) Serial.print("SUCCESS");
  else if (st == OpenThermResponseStatus::NONE) Serial.print("NONE");
  else if (st == OpenThermResponseStatus::INVALID) Serial.print("INVALID");
  else if (st == OpenThermResponseStatus::TIMEOUT) Serial.print("TIMEOUT");
  else Serial.print((int)st);

  Serial.print(" | CH=");
  Serial.print(boilerOn ? "ON" : "OFF");

  Serial.print(" | Commanded Temp=");
  Serial.print(commandedTemp);

  Serial.print(" | Raw OT Response HEX=0x");
  Serial.println(ot.getLastResponse(), HEX);


  // OLED
  updateDisplay();

  // Kis delay az UI miatt, de nem az OT ciklust szabályozza
  delay(50);
}
