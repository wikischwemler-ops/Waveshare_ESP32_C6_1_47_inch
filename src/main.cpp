// Flitz hüpft – ein kleines Jump'n'Run für das Waveshare ESP32-C6-LCD-1.47
// Steuerung: BOOT-Taste. Kurz tippen = kleiner Sprung, halten = hoher Sprung.

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Preferences.h>
#include <math.h>

// ---------- Hardware ----------
constexpr int PIN_MOSI = 6;
constexpr int PIN_SCLK = 7;
constexpr int PIN_CS   = 14;
constexpr int PIN_DC   = 15;
constexpr int PIN_RST  = 21;
constexpr int PIN_BL   = 22;
constexpr int PIN_RGB  = 8;
constexpr int PIN_BTN  = 9;    // BOOT-Taste
constexpr int BL_DUTY  = 130;  // ca. 50 % Helligkeit, das Board wird sonst heiß

// ---------- Spielfeld ----------
constexpr int W = 320, H = 172, T = 8;  // Bildschirm, Kachelgröße
constexpr int RING = 128;               // Spalten im Ringpuffer
constexpr int PW = 10, PH = 12;         // Spielerbox
constexpr int MAX_EN = 10;

Adafruit_ST7789 tft(&SPI, PIN_CS, PIN_DC, PIN_RST);
GFXcanvas16 *cv = nullptr;
Preferences prefs;

constexpr uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const uint16_t COL_GRASS = C(90, 200, 70), COL_GRASS_D = C(50, 150, 50);
const uint16_t COL_DIRT = C(150, 95, 55), COL_DIRT_D = C(115, 70, 40);
const uint16_t COL_WOOD = C(200, 140, 70), COL_WOOD_D = C(140, 90, 40), COL_WOOD_H = C(235, 185, 110);
const uint16_t COL_GOLD = C(255, 210, 40), COL_GOLD_H = C(255, 250, 180);
const uint16_t COL_BODY = C(40, 190, 200), COL_BELLY = C(170, 240, 240);
const uint16_t COL_DARK = C(20, 30, 40), COL_WHITE = 0xFFFF;
const uint16_t COL_ENEMY = C(150, 60, 190), COL_ENEMY_D = C(100, 30, 140);
const uint16_t COL_MTN = C(125, 135, 205), COL_MTN_H = C(200, 205, 240);
const uint16_t COL_HILL = C(110, 190, 120), COL_HILL_D = C(85, 160, 100);
const uint16_t COL_PINK = C(255, 120, 160);
uint16_t skyRow[H];

// ground: Höhe in Kacheln (0 = Graben), plat: Oberkante einer Holzplattform,
// coin: Münzreihe (0 = keine), deco: 1 Blume, 2 Grasbüschel
struct Col { uint8_t ground, plat, coin, deco; };
Col ring[RING];
int32_t genCol = 0;
int gGround = 3;

struct Enemy { float x, y, vx; bool alive; uint8_t squish; };
Enemy en[MAX_EN];

enum State { TITLE, PLAY, OVER };
State state = TITLE;

float cam = 0, speed = 1.5f;
float px, py, vy;
bool onGround = false, prevHeld = false;
int coyote = 0, jumpBuf = 0, holdT = 0;
uint32_t coins = 0, stomps = 0, score = 0, hiscore = 0;
uint32_t tick = 0, relTick = 0;
int overT = 0;
bool newHi = false;

volatile bool btnFlag = false;
void IRAM_ATTR onBtn() { btnFlag = true; }

int ledT = 0;
void led(uint8_t r, uint8_t g, uint8_t b, int t) {
  rgbLedWrite(PIN_RGB, r, g, b);
  ledT = t;
}

Col &col(int32_t c) { return ring[((c % RING) + RING) % RING]; }

float groundTop(int32_t c) {
  uint8_t g = col(c).ground;
  return g ? (float)(H - g * T) : 1e9f;
}

// ---------- Levelgenerator ----------
void put(uint8_t g, uint8_t p = 0, uint8_t coin = 0) {
  Col &c = col(genCol);
  c.ground = g;
  c.plat = p;
  c.coin = coin;
  c.deco = (g > 0 && random(100) < 20) ? 1 + random(2) : 0;
  genCol++;
}

void spawnEnemy(int32_t c) {
  if (col(c).ground == 0) return;
  for (auto &e : en) {
    if (!e.alive && e.squish == 0) {
      e.x = c * T;
      e.y = groundTop(c) - 10;
      e.vx = -0.45f;
      e.alive = true;
      return;
    }
  }
}

void genSegment() {
  float d = min(1.0f, genCol / 2000.0f);  // Schwierigkeit 0..1
  if (genCol < 30) { put(gGround); return; }

  int r = random(100);
  if (r < 28) {  // flaches Stück, evtl. Münzreihe und Gegner
    int len = random(5, 11);
    bool coinRow = random(100) < 45;
    int32_t start = genCol;
    for (int i = 0; i < len; i++)
      put(gGround, 0, (coinRow && i >= 1 && i < len - 1) ? gGround + 3 : 0);
    if (len >= 6 && random(100) < 30 + 40 * d) spawnEnemy(start + len / 2);
  } else if (r < 52) {  // Graben mit Münzbogen
    int len = random(2, 4 + (int)(2 * d));
    for (int i = 0; i < len; i++) {
      int arc = (int)roundf(sinf(PI * (i + 0.5f) / len) * 2);
      put(0, 0, gGround + 2 + arc);
    }
    gGround = constrain(gGround + (int)random(-1, 2), 2, 8);
    put(gGround);
    put(gGround);
  } else if (r < 68) {  // Stufe hoch oder runter
    int delta = (int)random(1, 3) * (random(2) ? 1 : -1);
    gGround = constrain(gGround + delta, 2, 8);
    int len = random(3, 7);
    for (int i = 0; i < len; i++) put(gGround);
  } else if (r < 84) {  // Plattform über dem Boden
    int len = random(8, 12);
    int p = gGround + 4;
    int32_t start = genCol;
    for (int i = 0; i < len; i++) {
      bool on = i >= 2 && i < len - 2;
      put(gGround, on ? p : 0, on ? p + 2 : 0);
    }
    if (random(100) < 25 + 40 * d) spawnEnemy(start + len - 2);
  } else {  // breiter Graben mit Plattform
    int len = random(6, 9);
    int p = gGround + 3;
    for (int i = 0; i < len; i++) {
      bool on = i >= 2 && i < len - 2;
      put(0, on ? p : 0, on ? p + 2 : 0);
    }
    put(gGround);
    put(gGround);
  }
}

void ensureGen() {
  while (genCol < (int32_t)(cam / T) + 50) genSegment();
}

void resetWorld() {
  genCol = 0;
  gGround = 3;
  cam = 0;
  speed = 1.5f;
  for (auto &e : en) { e.alive = false; e.squish = 0; }
  for (auto &c : ring) c = {0, 0, 0, 0};
  ensureGen();
  px = 60;
  py = groundTop(7) - PH;
  vy = 0;
  onGround = true;
  coyote = jumpBuf = holdT = 0;
  coins = stomps = score = 0;
  newHi = false;
}

void die() {
  state = OVER;
  overT = 0;
  led(60, 0, 0, 40);
  if (score > hiscore) {
    hiscore = score;
    newHi = true;
    prefs.putUInt("hi", hiscore);
  }
}

// ---------- Spiellogik (60 Schritte pro Sekunde) ----------
void stepPlay(bool pressed, bool held) {
  speed = 1.5f + min(1.6f, cam / 5000.0f);
  cam += speed;
  ensureGen();

  // Sprung mit Puffer und "Coyote Time" (kurz nach der Kante noch springen)
  if (pressed) jumpBuf = 8;
  if (onGround) coyote = 6;
  if (jumpBuf > 0 && coyote > 0) {
    vy = -4.6f;
    onGround = false;
    coyote = 0;
    jumpBuf = 0;
    holdT = 16;
  }
  if (jumpBuf > 0) jumpBuf--;

  // horizontal: Spieler läuft mit, Wände bremsen ihn
  float dx = speed + (cam + 60 - px) * 0.05f;
  if (dx < 0) dx = 0;
  float nx = px + dx;
  int32_t fc = (int32_t)floorf((nx + PW - 1) / T);
  if (py + PH > groundTop(fc) + 0.5f) nx = max(px, (float)(fc * T - PW));
  px = nx;

  // vertikal: halten = flacher Bogen, loslassen = schneller fallen
  if (!held) holdT = 0;
  float g = (held && holdT > 0 && vy < 0) ? 0.22f : 0.45f;
  if (holdT > 0) holdT--;
  vy = min(vy + g, 6.0f);
  float ny = py + vy;
  onGround = false;
  if (vy >= 0) {
    float feetOld = py + PH, feetNew = ny + PH, best = 1e9f;
    int32_t c0 = (int32_t)floorf((px + 1) / T), c1 = (int32_t)floorf((px + PW - 2) / T);
    for (int32_t c = c0; c <= c1; c++) {
      float s = groundTop(c);
      if (feetOld <= s + 0.01f && feetNew >= s) best = min(best, s);
      uint8_t p = col(c).plat;
      if (p) {
        s = H - p * T;
        if (feetOld <= s + 0.01f && feetNew >= s) best = min(best, s);
      }
    }
    if (best < 1e8f) { ny = best - PH; vy = 0; onGround = true; }
  }
  py = ny;
  if (!onGround && coyote > 0) coyote--;

  // Münzen
  int32_t c0 = (int32_t)floorf(px / T), c1 = (int32_t)floorf((px + PW - 1) / T);
  for (int32_t c = c0; c <= c1; c++) {
    Col &cc = col(c);
    if (!cc.coin) continue;
    float y0 = H - cc.coin * T, y1 = y0 + T;
    if (py < y1 && py + PH > y0) {
      cc.coin = 0;
      coins++;
      led(40, 30, 0, 6);
    }
  }

  // Gegner
  for (auto &e : en) {
    if (!e.alive) { if (e.squish) e.squish--; continue; }
    if (e.x < cam - 20) { e.alive = false; continue; }
    if (e.x > cam + W + 10) continue;  // erst loslaufen, wenn sichtbar
    float enx = e.x + e.vx;
    int32_t front = (int32_t)floorf((e.vx < 0 ? enx : enx + 9) / T);
    int32_t here = (int32_t)floorf((e.x + 5) / T);
    if (col(front).ground != col(here).ground) e.vx = -e.vx;
    else e.x = enx;
    e.y = groundTop(here) - 10;

    if (px < e.x + 10 && px + PW > e.x && py < e.y + 10 && py + PH > e.y) {
      if (vy > 0 && py + PH - vy <= e.y + 5) {  // von oben drauf
        e.alive = false;
        e.squish = 30;
        stomps++;
        vy = held ? -5.0f : -3.5f;
        holdT = 0;
        led(0, 40, 10, 8);
      } else {
        die();
        return;
      }
    }
  }

  score = (uint32_t)(cam / T) + coins * 10 + stomps * 25;
  if (py > H + 10 || px + PW < cam) die();
}

void step() {
  tick++;
  bool raw = digitalRead(PIN_BTN) == LOW;
  bool pressed = false;
  if (raw && !prevHeld && tick - relTick > 3) pressed = true;
  if (!raw && prevHeld) relTick = tick;
  if (btnFlag && !raw && !prevHeld && tick - relTick > 3) pressed = true;  // sehr kurzer Tipp
  btnFlag = false;
  prevHeld = raw;

  if (ledT > 0 && --ledT == 0) rgbLedWrite(PIN_RGB, 0, 0, 0);

  switch (state) {
    case TITLE:
      cam += 0.8f;
      ensureGen();
      if (pressed) { resetWorld(); state = PLAY; }
      break;
    case PLAY:
      stepPlay(pressed, raw);
      break;
    case OVER:
      overT++;
      if (pressed && overT > 45) { resetWorld(); state = PLAY; }
      break;
  }
}

// ---------- Zeichnen ----------
void drawBackground() {
  uint16_t *buf = cv->getBuffer();
  for (int y = 0; y < H; y++) {
    uint16_t c = skyRow[y];
    uint16_t *row = buf + y * W;
    for (int x = 0; x < W; x++) row[x] = c;
  }
  for (int i = 0; i < 4; i++) {  // Wolken
    float span = W + 80;
    float x = fmodf(i * 97.0f + 30 - cam * 0.15f, span);
    if (x < 0) x += span;
    int cx = (int)x - 40, cy = 16 + (i * 23) % 40;
    cv->fillCircle(cx, cy, 7, COL_WHITE);
    cv->fillCircle(cx + 10, cy - 4, 9, COL_WHITE);
    cv->fillCircle(cx + 21, cy, 7, COL_WHITE);
    cv->fillRect(cx, cy, 22, 8, COL_WHITE);
  }
  for (int x = 0; x < W; x++) {  // Berge und Hügel (Parallax)
    float a = x + cam * 0.2f;
    int h = 58 + (int)(18 * sinf(a * 0.021f) + 9 * sinf(a * 0.053f + 1.3f));
    cv->drawFastVLine(x, H - h, h, COL_MTN);
    if (h > 70) cv->drawFastVLine(x, H - h, (h - 70) / 2 + 1, COL_MTN_H);
    float b = x + cam * 0.45f;
    int h2 = 34 + (int)(10 * sinf(b * 0.035f + 0.7f) + 5 * sinf(b * 0.09f));
    cv->drawFastVLine(x, H - h2, h2, COL_HILL);
    cv->drawPixel(x, H - h2, COL_HILL_D);
  }
}

void drawWorld() {
  int ci = (int)cam;
  int32_t c0 = ci / T - 1, c1 = c0 + W / T + 2;
  for (int32_t c = c0; c <= c1; c++) {
    Col &cc = col(c);
    int sx = c * T - ci;
    if (cc.ground) {
      int top = H - cc.ground * T;
      cv->fillRect(sx, top, T, H - top, COL_DIRT);
      for (int y = top + 5; y < H; y += 4) {
        int h = (int)((c * 7 + y * 3) & 7);
        if (h < 2) cv->fillRect(sx + (h * 3 + y) % 6, y, 2, 2, COL_DIRT_D);
      }
      cv->fillRect(sx, top, T, 3, COL_GRASS);
      cv->drawFastHLine(sx, top + 3, T, COL_GRASS_D);
      if (col(c - 1).ground < cc.ground) cv->drawFastVLine(sx, top, H - top, COL_DIRT_D);
      if (col(c + 1).ground < cc.ground) cv->drawFastVLine(sx + T - 1, top, H - top, COL_DIRT_D);
      if (cc.deco == 1) {
        cv->drawFastVLine(sx + 4, top - 4, 4, COL_GRASS_D);
        cv->fillCircle(sx + 4, top - 5, 1, COL_PINK);
      } else if (cc.deco == 2) {
        cv->drawLine(sx + 2, top, sx + 1, top - 3, COL_GRASS_D);
        cv->drawLine(sx + 4, top, sx + 4, top - 4, COL_GRASS_D);
        cv->drawLine(sx + 6, top, sx + 7, top - 3, COL_GRASS_D);
      }
    }
    if (cc.plat) {
      int top = H - cc.plat * T;
      cv->fillRect(sx, top, T, 6, COL_WOOD);
      cv->drawFastHLine(sx, top, T, COL_WOOD_H);
      cv->drawFastHLine(sx, top + 5, T, COL_WOOD_D);
      if ((c & 1) == 0) cv->drawFastVLine(sx, top + 1, 4, COL_WOOD_D);
    }
    if (cc.coin) {
      int cx = sx + T / 2, cy = H - cc.coin * T + T / 2;
      int w = 1 + (int)(fabsf(cosf(tick * 0.08f + c * 0.5f)) * 3);
      cv->fillRoundRect(cx - w, cy - 4, w * 2, 8, w, COL_GOLD);
      if (w > 1) cv->drawFastVLine(cx - w + 1, cy - 2, 4, COL_GOLD_H);
    }
  }
}

void drawEnemy(const Enemy &e) {
  int x = (int)(e.x - cam), y = (int)e.y;
  if (!e.alive) {
    cv->fillRoundRect(x, y + 6, 10, 4, 2, COL_ENEMY);
    return;
  }
  int cx = x + 5, cy = y + 5;
  int wob = (tick / 8) & 1;
  cv->fillTriangle(cx - 2, cy - 4, cx + 2, cy - 4, cx, cy - 8 - wob, COL_ENEMY_D);
  cv->fillTriangle(cx - 4, cy - 3, cx - 4, cy + 1, cx - 8 - wob, cy - 2, COL_ENEMY_D);
  cv->fillTriangle(cx + 4, cy - 3, cx + 4, cy + 1, cx + 8 + wob, cy - 2, COL_ENEMY_D);
  cv->fillCircle(cx, cy, 5, COL_ENEMY);
  int dir = e.vx < 0 ? -1 : 1;
  cv->fillRect(cx - 3 + dir, cy - 2, 2, 3, COL_WHITE);
  cv->fillRect(cx + 1 + dir, cy - 2, 2, 3, COL_WHITE);
  cv->drawPixel(cx - 3 + dir + (dir > 0), cy - 1, COL_DARK);
  cv->drawPixel(cx + 1 + dir + (dir > 0), cy - 1, COL_DARK);
  cv->drawLine(cx - 4 + dir, cy - 4, cx - 1 + dir, cy - 3, COL_DARK);
  cv->drawLine(cx + 4 + dir, cy - 4, cx + 1 + dir, cy - 3, COL_DARK);
  int s = (tick / 6) & 1;
  cv->fillRect(cx - 4 + s, y + 9, 3, 1, COL_DARK);
  cv->fillRect(cx + 1 - s, y + 9, 3, 1, COL_DARK);
}

void drawPlayer() {
  int x = (int)(px - cam), y = (int)py;
  int cx = x + 5, cy = y + 5;
  int s = ((int)(cam / 6)) & 1;
  if (onGround) {
    cv->fillRect(cx - 4 + s, y + 10, 3, 2, COL_DARK);
    cv->fillRect(cx + 1 - s, y + 10, 3, 2, COL_DARK);
  } else {
    cv->fillRect(cx - 4, y + 9, 3, 2, COL_DARK);
    cv->fillRect(cx + 2, y + 9, 3, 2, COL_DARK);
  }
  int bob = onGround ? s : (vy < 0 ? 1 : -1);
  cv->drawLine(cx - 1, cy - 4, cx - 3, cy - 8 - bob, COL_DARK);
  cv->fillCircle(cx - 3, cy - 9 - bob, 1, COL_GOLD);
  cv->fillCircle(cx, cy, 5, COL_BODY);
  cv->fillCircle(cx - 1, cy + 2, 2, COL_BELLY);
  cv->fillCircle(cx + 2, cy - 1, 2, COL_WHITE);
  cv->drawFastVLine(cx + 3, cy - 2, 2, COL_DARK);
  cv->drawPixel(cx + 4, cy + 2, COL_PINK);
}

void drawText(const char *s, int x, int y, uint8_t size, uint16_t c) {
  cv->setTextSize(size);
  cv->setTextColor(COL_DARK);
  cv->setCursor(x + 1, y + 1);
  cv->print(s);
  cv->setTextColor(c);
  cv->setCursor(x, y);
  cv->print(s);
}

void drawCentered(const char *s, int y, uint8_t size, uint16_t c) {
  drawText(s, (W - (int)strlen(s) * 6 * size) / 2, y, size, c);
}

void render() {
  char buf[40];
  drawBackground();
  drawWorld();
  for (auto &e : en)
    if ((e.alive || e.squish) && e.x > cam - 12 && e.x < cam + W) drawEnemy(e);

  if (state == TITLE) {
    drawCentered("FLITZ HUEPFT", 30, 3, COL_GOLD);
    if ((tick / 30) & 1) drawCentered("BOOT-Taste druecken", 72, 1, COL_WHITE);
    drawCentered("tippen = klein   halten = hoch", 88, 1, COL_WHITE);
    snprintf(buf, sizeof buf, "Rekord: %lu", (unsigned long)hiscore);
    drawCentered(buf, 104, 1, COL_GOLD_H);
    return;
  }

  drawPlayer();
  cv->fillRoundRect(6, 6, 6, 8, 3, COL_GOLD);
  snprintf(buf, sizeof buf, "x%lu", (unsigned long)coins);
  drawText(buf, 15, 6, 1, COL_WHITE);
  snprintf(buf, sizeof buf, "%lu m", (unsigned long)(cam / T));
  drawCentered(buf, 6, 1, COL_WHITE);
  snprintf(buf, sizeof buf, "%lu", (unsigned long)score);
  drawText(buf, W - (int)strlen(buf) * 12 - 6, 4, 2, COL_GOLD);

  if (state == OVER) {
    cv->fillRoundRect(60, 36, 200, 100, 10, COL_DARK);
    cv->drawRoundRect(60, 36, 200, 100, 10, COL_WHITE);
    drawCentered("AUTSCH!", 46, 3, COL_PINK);
    snprintf(buf, sizeof buf, "Punkte: %lu", (unsigned long)score);
    drawCentered(buf, 78, 1, COL_WHITE);
    if (newHi) drawCentered("NEUER REKORD!", 92, 1, COL_GOLD);
    else {
      snprintf(buf, sizeof buf, "Rekord: %lu", (unsigned long)hiscore);
      drawCentered(buf, 92, 1, COL_GOLD_H);
    }
    if (overT > 45 && ((tick / 30) & 1)) drawCentered("BOOT = nochmal", 114, 1, COL_WHITE);
  }
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), onBtn, FALLING);
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
  for (int y = 0; y < H; y++) {
    float t = y / (float)(H - 1);
    skyRow[y] = C((uint8_t)(110 + t * 110), (uint8_t)(170 + t * 60), (uint8_t)(250 - t * 20));
  }

  prefs.begin("flitz", false);
  hiscore = prefs.getUInt("hi", 0);
  randomSeed(esp_random());
  resetWorld();
}

uint32_t lastUs = 0;
float acc = 0;

void loop() {
  uint32_t now = micros();
  if (!lastUs) lastUs = now;
  acc += (now - lastUs) * 0.00006f;  // 60 Logikschritte pro Sekunde
  lastUs = now;
  if (acc > 4) acc = 4;
  while (acc >= 1) { step(); acc -= 1; }
  render();
  tft.drawRGBBitmap(0, 0, cv->getBuffer(), W, H);
}
