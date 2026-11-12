// Haptik-Test fuer eine iPhone-Taptic-Engine am Waveshare ESP32-C6-LCD-1.47
//
// Die Taptic Engine ist ein LRA (Spule mit Federmasse). Sie haengt ueber einen
// Logic-Level-MOSFET an 5 V, der GPIO schaltet nur das Gate:
//
//   5 V ──┬── Taptic Engine ──┬── Drain (AO3400)
//         └──── 1N4148 ───────┘   (Kathode an 5 V)
//   PIN_HAPTIK ── 100 Ω ── Gate,  10 kΩ Gate→GND,  Source→GND
//
// Ein esp_timer schaltet den Pin im Takt der eingestellten Frequenz und hoert nach
// einer genauen Anzahl Perioden auf. Die Hauptschleife wird dabei nicht blockiert.
//
// Bedienung mit der BOOT-Taste:
//   jeder Druck        -> Klick bzw. Brummen ausloesen
//   halten (> 0,6 s)   -> Frequenz +10 Hz, laeuft weiter solange gehalten (100..300 Hz)
//   zweimal schnell    -> Modus wechseln (Klick 2 / 3 / 5 Perioden, Brummen 0,4 s)
// Frequenz und Modus bleiben nach dem Ausschalten erhalten (NVS).

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <driver/gpio.h>

// ---------- Hardware ----------
constexpr int PIN_MOSI = 6;
constexpr int PIN_SCLK = 7;
constexpr int PIN_CS   = 14;
constexpr int PIN_DC   = 15;
constexpr int PIN_RST  = 21;
constexpr int PIN_BL   = 22;
constexpr int PIN_RGB  = 8;
constexpr int PIN_BTN  = 9;    // BOOT-Taste
constexpr int PIN_HAPTIK = 2;  // Gate des MOSFET (ueber 100 Ohm)
constexpr int BL_DUTY  = 130;

constexpr int F_MIN = 100, F_MAX = 300, F_SCHRITT = 10;
constexpr uint32_t LANG_MS = 600, WIEDERHOL_MS = 350, DOPPEL_MS = 300;

constexpr int W = 320, H = 172;

Adafruit_ST7789 tft(&SPI, PIN_CS, PIN_DC, PIN_RST);
GFXcanvas16 *cv = nullptr;
Preferences prefs;

constexpr uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t COL_BG = C(15, 20, 35), COL_TXT = 0xFFFF, COL_DIM = C(140, 150, 175);
const uint16_t COL_ACC = C(255, 200, 40), COL_GO = C(60, 210, 120);

// ---------- Modi ----------
struct Modus { const char *name; int perioden; int dauerMs; };  // dauerMs > 0 = Brummen
const Modus MODI[] = {
  {"Klick 2", 2, 0},
  {"Klick 3", 3, 0},
  {"Klick 5", 5, 0},
  {"Brummen", 0, 400},
};
constexpr int ANZ_MODI = sizeof(MODI) / sizeof(MODI[0]);

int frequenz = 170;
int modus = 1;
uint32_t ausgeloest = 0;

// ---------- Ansteuerung ueber esp_timer ----------
esp_timer_handle_t hTimer = nullptr;
volatile int restHalbwellen = 0;
volatile bool pegel = false;
volatile bool aktiv = false;

void halbwelle(void *) {
  if (--restHalbwellen <= 0) {
    gpio_set_level((gpio_num_t)PIN_HAPTIK, 0);
    pegel = false;
    esp_timer_stop(hTimer);
    aktiv = false;
    return;
  }
  pegel = !pegel;
  gpio_set_level((gpio_num_t)PIN_HAPTIK, pegel);
}

void ausloesen() {
  if (aktiv) esp_timer_stop(hTimer);
  const Modus &m = MODI[modus];
  int perioden = m.dauerMs > 0 ? max(1, frequenz * m.dauerMs / 1000) : m.perioden;
  uint64_t halbUs = 500000ULL / frequenz;
  restHalbwellen = perioden * 2;
  pegel = true;
  aktiv = true;
  gpio_set_level((gpio_num_t)PIN_HAPTIK, 1);
  esp_timer_start_periodic(hTimer, halbUs);
  ausgeloest++;
  rgbLedWrite(PIN_RGB, 0, 25, 8);
  Serial.printf("Ausgeloest: %d Hz, %s (%d Perioden, %.1f ms)\n", frequenz, m.name, perioden,
                perioden * 1000.0f / frequenz);
}

// ---------- Anzeige ----------
void zentriert(const char *s, int y, int gr, uint16_t farbe) {
  int16_t x1, y1;
  uint16_t w, h;
  cv->setTextSize(gr);
  cv->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  cv->setTextColor(farbe);
  cv->setCursor((W - (int)w) / 2, y);
  cv->print(s);
}

void zeichnen() {
  char buf[48];
  cv->fillScreen(COL_BG);
  zentriert("HAPTIK-TEST", 8, 2, COL_DIM);

  snprintf(buf, sizeof buf, "%d Hz", frequenz);
  zentriert(buf, 40, 6, COL_ACC);

  const Modus &m = MODI[modus];
  if (m.dauerMs > 0) snprintf(buf, sizeof buf, "%s %d ms", m.name, m.dauerMs);
  else snprintf(buf, sizeof buf, "%s (%.1f ms)", m.name, m.perioden * 1000.0f / frequenz);
  zentriert(buf, 98, 2, COL_GO);

  snprintf(buf, sizeof buf, "GPIO %d   Pulse: %lu", PIN_HAPTIK, (unsigned long)ausgeloest);
  zentriert(buf, 124, 1, COL_DIM);
  zentriert("Druck = ausloesen   Halten = +10 Hz", 142, 1, COL_TXT);
  zentriert("2x schnell = Modus wechseln", 156, 1, COL_TXT);
  tft.drawRGBBitmap(0, 0, cv->getBuffer(), W, H);
}

void speichern() {
  prefs.putInt("f", frequenz);
  prefs.putInt("m", modus);
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);

  // Zuerst den Ausgang sicher auf LOW, damit die Spule nie dauerhaft Strom bekommt
  gpio_reset_pin((gpio_num_t)PIN_HAPTIK);
  gpio_set_direction((gpio_num_t)PIN_HAPTIK, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_HAPTIK, 0);

  esp_timer_create_args_t args = {};
  args.callback = halbwelle;
  args.name = "haptik";
  esp_timer_create(&args, &hTimer);

  pinMode(PIN_BTN, INPUT_PULLUP);
  ledcAttach(PIN_BL, 5000, 8);
  ledcWrite(PIN_BL, 0);
  rgbLedWrite(PIN_RGB, 0, 0, 0);

  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
  tft.init(172, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.fillScreen(0);
  ledcWrite(PIN_BL, BL_DUTY);

  cv = new GFXcanvas16(W, H);
  if (!cv->getBuffer()) {
    Serial.println("Zu wenig RAM fuer Bildpuffer");
    while (true) delay(1000);
  }

  prefs.begin("haptik", false);
  frequenz = constrain(prefs.getInt("f", frequenz), F_MIN, F_MAX);
  modus = constrain(prefs.getInt("m", modus), 0, ANZ_MODI - 1);

  Serial.printf("Haptik-Test bereit: GPIO %d, %d Hz, %s\n", PIN_HAPTIK, frequenz, MODI[modus].name);
  zeichnen();
}

bool gedrueckt = false;
uint32_t druckBeginn = 0, letzterSchritt = 0, letztesLoslassen = 0;
bool langGedrueckt = false;
uint32_t ledAus = 0;

void loop() {
  uint32_t jetzt = millis();
  bool unten = digitalRead(PIN_BTN) == LOW;

  if (unten && !gedrueckt) {               // Taste neu gedrueckt
    gedrueckt = true;
    langGedrueckt = false;
    druckBeginn = jetzt;
    ausloesen();
    ledAus = jetzt + 120;
    if (jetzt - letztesLoslassen < DOPPEL_MS) {  // Doppeldruck
      modus = (modus + 1) % ANZ_MODI;
      speichern();
      letztesLoslassen = 0;                // dritter Druck zaehlt nicht als Doppeldruck
    }
    zeichnen();
  } else if (unten && gedrueckt) {         // Taste gehalten
    bool schritt = false;
    if (!langGedrueckt && jetzt - druckBeginn > LANG_MS) {
      langGedrueckt = true;
      schritt = true;
    } else if (langGedrueckt && jetzt - letzterSchritt > WIEDERHOL_MS) {
      schritt = true;
    }
    if (schritt) {
      letzterSchritt = jetzt;
      frequenz += F_SCHRITT;
      if (frequenz > F_MAX) frequenz = F_MIN;
      speichern();
      ausloesen();                         // gleich mit der neuen Frequenz fuehlen
      ledAus = jetzt + 120;
      zeichnen();
    }
  } else if (!unten && gedrueckt) {        // Taste losgelassen
    gedrueckt = false;
    letztesLoslassen = langGedrueckt ? 0 : jetzt;
  }

  if (ledAus && (int32_t)(jetzt - ledAus) >= 0) {
    rgbLedWrite(PIN_RGB, 0, 0, 0);
    ledAus = 0;
  }
  delay(5);
}
