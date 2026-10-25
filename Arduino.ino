/*
  DESK LCD  v2  -  Arduino Nano  (display unit, RECEIVE ONLY)
  LCD : RS=7 EN=8 D4=9 D5=10 D6=11 D7=12      BTN : D2 -> GND (INPUT_PULLUP)
  LINK: ESP8266 TX -> Arduino RX, 115200 baud. The Arduino never transmits to the ESP.
        (Arduino TX only prints a status line to the USB serial monitor.)

  The Arduino owns ALL text and animation. The ESP only reports what happens.

  FRAME  (binary):  A5 | CMD | LEN | PAYLOAD[LEN] | CHK     CHK = CMD ^ LEN ^ payload bytes
    10 WIFI  [s]            s: 0 connecting, 1 connected, 2 failed/lost
    12 LED   [n, on]        n: 0-4
    13 SESS  [on]
    14 SHUT  [1=shutdown, 0=wake]
    15 BEAT  [mask, mode, wifi, ip0, ip1, ip2, ip3]    every 5 s (mode: 0 normal, 1 session, 2 shutdown)

  Serial is polled during every animation step, so no frame is ever lost.
  Events are queued and played one at a time with a 1 s cooldown.
*/

#include <LiquidCrystal.h>
#include <avr/pgmspace.h>

#define BTN_PIN       2
#define DEB_MS        35UL
#define LONG_MS       950UL
#define GAP_MS        1000UL     // cooldown between queued scenes
#define IDLE_MS       12000UL    // idle starts after this much quiet
#define SESS_IDLE_MS  4000UL
#define IP_MS         20000UL
#define LOG_MS        9000UL
#define MAINT_MS      600000UL   // 10 min silent clean-up

#define HDR     0xA5
#define MAXP    7
#define C_WIFI  0x10
#define C_LED   0x12
#define C_SESS  0x13
#define C_SHUT  0x14
#define C_BEAT  0x15

enum { E_LED = 1, E_SON, E_SOFF, E_SHUT, E_WAKE, E_WIFI };

LiquidCrystal lcd(7, 8, 9, 10, 11, 12);

// custom chars: 0 block, 1 spark, 2 moon, 3 sun, 4 heart, 5 note, 6 bar, 7 dot
const uint8_t CC[8][8] PROGMEM = {
  {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F},
  {0x04,0x0E,0x1F,0x0E,0x04,0x00,0x00,0x00},
  {0x07,0x0F,0x1F,0x1F,0x1F,0x0F,0x07,0x00},
  {0x0E,0x1F,0x1F,0x1F,0x0E,0x00,0x00,0x00},
  {0x00,0x0A,0x1F,0x1F,0x0E,0x04,0x00,0x00},
  {0x02,0x03,0x02,0x0E,0x1E,0x0C,0x00,0x00},
  {0x1F,0x1F,0x00,0x00,0x00,0x1F,0x1F,0x00},
  {0x00,0x00,0x0E,0x1F,0x0E,0x00,0x00,0x00},
};

// ───────────────────────── texts (all live here) ─────────────────────────
const char S_HOME0[] PROGMEM = "  ambient ctrl  ";
const char S_HOME1[] PROGMEM = "  awaiting cmd  ";
const char S_BOOT1[] PROGMEM = "  starting up~  ";
const char S_SES0[]  PROGMEM = "\x01 SESSION LIVE \x01";
const char S_SES1[]  PROGMEM = " ALL SYSTEMS:GO!";
const char S_SD0[]   PROGMEM = "  system mode:  ";
const char S_SD1[]   PROGMEM = "  \x02 shutdown \x02  ";
const char S_SEND[]  PROGMEM = "   session ended   ";
const char S_SOFF0[] PROGMEM = "  session : off ";
const char S_SOFF1[] PROGMEM = "  lights cleared";
const char S_SHDN[]  PROGMEM = "  !!  shutting down  !!  ";
const char S_WSET[]  PROGMEM = " \x01 wifi setup \x01 ";
const char S_CONN[]  PROGMEM = "  connecting";
const char S_WON0[]  PROGMEM = " wifi : online!  ";
const char S_WON1[]  PROGMEM = " connected ok~  ";
const char S_WOF0[]  PROGMEM = " wifi : offline! ";
const char S_WOF1[]  PROGMEM = "  no connection ";
const char S_LWON0[] PROGMEM = " wifi : online   ";
const char S_LWOF0[] PROGMEM = " wifi : offline  ";
const char S_LWLK0[] PROGMEM = " wifi : linking  ";
const char S_LWLK1[] PROGMEM = "  please wait~  ";
const char S_LWUN0[] PROGMEM = " wifi : unknown  ";
const char S_LWUN1[] PROGMEM = " no esp signal~ ";
const char S_IP0[]   PROGMEM = " \x01 ip address \x01 ";
const char S_NOIP[]  PROGMEM = "no ip yet";
const char S_LOGE0[] PROGMEM = "  log is empty~ ";
const char S_LOGE1[] PROGMEM = "  no events yet ";

const char N0[] PROGMEM = "PRIMARY LIGHT";
const char N1[] PROGMEM = "ACCENT LIGHT";
const char N2[] PROGMEM = "AMBIENT LIGHT";
const char N3[] PROGMEM = "NIGHT LIGHT";
const char N4[] PROGMEM = "POWER STRIP";
const char* const NAMES[] PROGMEM = { N0, N1, N2, N3, N4 };

const char TN0[] PROGMEM = "\x01 lit up! go!";
const char TN1[] PROGMEM = "\x01 vibe: on!";
const char TN2[] PROGMEM = "\x04 mood: set!";
const char TN3[] PROGMEM = "\x02 nite mode!";
const char TN4[] PROGMEM = "\x01 surge: on!";
const char* const TON[] PROGMEM = { TN0, TN1, TN2, TN3, TN4 };

const char TF0[] PROGMEM = "lights: out~";
const char TF1[] PROGMEM = "accent: off~";
const char TF2[] PROGMEM = "mood: offline";
const char TF3[] PROGMEM = "sweet dreams~";
const char TF4[] PROGMEM = "powered down";
const char* const TOFF[] PROGMEM = { TF0, TF1, TF2, TF3, TF4 };

const char im00[] PROGMEM = " \x05 lofi desk \x05 ";
const char im01[] PROGMEM = "  all quiet...  ";
const char im02[] PROGMEM = "  stay cozy~    ";
const char im03[] PROGMEM = "  breathe easy  ";
const char im04[] PROGMEM = " late night mode";
const char im05[] PROGMEM = "  good vibes~   ";
const char im06[] PROGMEM = "  midnight oil~ ";
const char im07[] PROGMEM = "  focus zone~   ";
const char im08[] PROGMEM = "  ambient ctrl  ";
const char im09[] PROGMEM = "  tap for logs  ";
const char im10[] PROGMEM = " hold for ip~   ";
const char im11[] PROGMEM = "  systems ok~   ";
const char im12[] PROGMEM = "  rest. create. ";
const char im13[] PROGMEM = "  night owl~    ";
const char im14[] PROGMEM = "  slow it down~ ";
const char im15[] PROGMEM = "  ambient glow~ ";
const char im16[] PROGMEM = "  in the zone~  ";
const char im17[] PROGMEM = " \x04 still here \x04 ";
const char im18[] PROGMEM = "  minimal desk  ";
const char im19[] PROGMEM = " \x02 drift off... ";
const char im20[] PROGMEM = "  make it cozy~ ";
const char im21[] PROGMEM = " \x01 stars align \x01";
const char im22[] PROGMEM = "  ctrl: zen~    ";
const char im23[] PROGMEM = "  signal: ok~   ";
const char im24[] PROGMEM = "  vibe: locked  ";
const char im25[] PROGMEM = " \x05 note to self ";
const char im26[] PROGMEM = "  keep it lit~  ";
const char im27[] PROGMEM = " \x03 morning mode";
const char im28[] PROGMEM = "  be still~     ";
const char im29[] PROGMEM = "  hum along~    ";
const char* const IMSGS[] PROGMEM = {
  im00,im01,im02,im03,im04,im05,im06,im07,im08,im09,
  im10,im11,im12,im13,im14,im15,im16,im17,im18,im19,
  im20,im21,im22,im23,im24,im25,im26,im27,im28,im29
};
#define IMSG_N 30

// ───────────────────────────── state ─────────────────────────────
uint8_t  fb[2][16];            // what we want on the LCD
uint8_t  cur[2][16];           // what the LCD currently shows
uint8_t  sv[2][16];            // screen saved while an overlay (log / ip) is open

uint8_t  ledMask = 0, seenMask = 0, lcnt[5] = {0,0,0,0,0};
uint8_t  ip4[4]  = {0,0,0,0};
uint8_t  wifiSt  = 255;        // 0 connecting, 1 ok, 2 failed, 255 unknown
uint8_t  rxMode  = 0, rxWifi = 255;
bool     syncNew = false;
uint32_t lastSync = 0;

bool     session = false, sleeping = false, homeDirty = false;
bool     inIP = false, inLog = false;
uint8_t  logIdx = 0;
uint32_t ipT = 0, logT = 0;

bool     btnShort = false, btnLong = false;

uint32_t lastAct = 0, sceneEndT = 0, maintT = 0;
bool     idleOn = false, blinkOn = true;
uint32_t idleT = 0, noteT = 0;
uint8_t  idleMsg = 0, idleTk = 0, idleStyle = 0, ph = 0;
int8_t   notePos = 1, noteDir = 1;
int8_t   sA = 0, sB = 15;
bool     sAd = true, sBd = false;
uint16_t rs = 0x1D2B;

// event queue
struct Ev { uint8_t t; uint8_t a; };
#define EVN 8
Ev      evq[EVN];
uint8_t evN = 0;

// frame parser
uint8_t  rb[12];
uint8_t  rn = 0;
uint32_t lastRx = 0;

// ─────────────────────────── event queue ───────────────────────────
void evDel(uint8_t i) {
  for (uint8_t j = i; j + 1 < evN; j++) evq[j] = evq[j + 1];
  if (evN) evN--;
}

void evPush(uint8_t t, uint8_t a) {
  if (t == E_LED) {
    for (uint8_t i = 0; i < evN; i++)
      if (evq[i].t == E_LED && (evq[i].a & 0x7F) == (a & 0x7F)) { evq[i].a = a; return; }
  } else if (t == E_SON || t == E_SOFF || t == E_SHUT) {
    for (uint8_t i = 0; i < evN;) {
      uint8_t x = evq[i].t;
      if (x == E_LED || (t == E_SHUT && (x == E_SON || x == E_SOFF))) evDel(i); else i++;
    }
  } else if (t == E_WIFI) {
    for (uint8_t i = 0; i < evN;) {
      if (evq[i].t == E_WIFI && (evq[i].a == a || a != 0)) evDel(i); else i++;
    }
  }
  if (evN >= EVN) evDel(0);
  evq[evN].t = t; evq[evN].a = a; evN++;
}

void evKeepLastLED() {
  int8_t last = -1;
  for (uint8_t i = 0; i < evN; i++) if (evq[i].t == E_LED) last = (int8_t)i;
  for (uint8_t i = 0; i < evN;) {
    if (evq[i].t == E_LED && (int8_t)i != last) { evDel(i); last--; } else i++;
  }
}

// ───────────────────────── frame handling ─────────────────────────
void ipStr(char* b) {
  snprintf(b, 17, "%u.%u.%u.%u", (unsigned)ip4[0], (unsigned)ip4[1], (unsigned)ip4[2], (unsigned)ip4[3]);
}

// Only updates data + queues events. Never touches the LCD.
void handle(uint8_t cmd, const uint8_t* p, uint8_t len) {
  switch (cmd) {
    case C_WIFI:
      if (len >= 1 && p[0] <= 2 && p[0] != wifiSt) {
        wifiSt = p[0];
        evPush(E_WIFI, p[0]);
        Serial.print(F("[desk] wifi=")); Serial.println(p[0]);
      }
      break;

    case C_LED:
      if (len >= 2 && p[0] < 5) {
        uint8_t n = p[0], on = p[1] ? 1 : 0;
        if (on) ledMask |= (uint8_t)(1 << n); else ledMask &= (uint8_t)~(1 << n);
        if (!sleeping) { seenMask |= (uint8_t)(1 << n); if (lcnt[n] < 99) lcnt[n]++; }
        evPush(E_LED, (uint8_t)(n | (on << 7)));
        Serial.print(F("[desk] out")); Serial.print(n); Serial.println(on ? F(" on") : F(" off"));
      }
      break;

    case C_SESS:
      if (len >= 1) {
        if (p[0]) { ledMask = 0x1F; if (!sleeping) seenMask = 0x1F; evPush(E_SON, 0); }
        else      { ledMask = 0;    evPush(E_SOFF, 0); }
        Serial.println(p[0] ? F("[desk] session on") : F("[desk] session off"));
      }
      break;

    case C_SHUT:
      if (len >= 1) {
        if (p[0]) { ledMask = 0; evPush(E_SHUT, 0); }
        else      { evPush(E_WAKE, 0); }
        Serial.println(p[0] ? F("[desk] shutdown") : F("[desk] wake"));
      }
      break;

    case C_BEAT:
      if (len >= 7) {
        rxMode = p[1]; rxWifi = p[2]; lastSync = millis(); syncNew = true;
        if (p[1] != 2) {
          ledMask = p[0] & 0x1F;
          for (uint8_t i = 0; i < 4; i++) ip4[i] = p[3 + i];
          if (!sleeping) seenMask = 0x1F;
        }
        char b[17];
        snprintf(b, 17, "%u.%u.%u.%u", (unsigned)p[3], (unsigned)p[4], (unsigned)p[5], (unsigned)p[6]);
        Serial.print(F("[desk] ip=")); Serial.print(b);
        Serial.print(F(" wifi=")); Serial.print(p[2]);
        Serial.print(F(" mode=")); Serial.print(p[1]);
        Serial.print(F(" out="));
        for (uint8_t i = 0; i < 5; i++) Serial.print((p[0] >> i) & 1);
        Serial.println();
      }
      break;
  }
}

void rbShift(uint8_t k) {
  if (k >= rn) { rn = 0; return; }
  memmove(rb, rb + k, rn - k);
  rn -= k;
}

// Resyncing parser: a bad checksum only drops one byte and rescans.
void feed(uint8_t b) {
  uint32_t now = millis();
  if (rn && (uint32_t)(now - lastRx) > 50UL) rn = 0;   // stale partial frame
  lastRx = now;
  if (rn >= sizeof(rb)) rbShift(1);
  rb[rn++] = b;
  while (rn) {
    if (rb[0] != HDR) { rbShift(1); continue; }
    if (rn < 3) return;
    uint8_t len = rb[2];
    if (len > MAXP) { rbShift(1); continue; }
    if (rn < (uint8_t)(4 + len)) return;
    uint8_t k = rb[1] ^ rb[2];
    for (uint8_t i = 0; i < len; i++) k ^= rb[3 + i];
    if (k == rb[3 + len]) {
      uint8_t cmd = rb[1], pl[MAXP];
      memcpy(pl, rb + 3, len);
      rbShift((uint8_t)(4 + len));
      handle(cmd, pl, len);
    } else {
      rbShift(1);
    }
  }
}

void pollSerial() {
  while (Serial.available()) feed((uint8_t)Serial.read());
}

// ───────────────────────────── button ─────────────────────────────
void pollBtn() {
  static bool     raw = HIGH, stable = HIGH, longDone = false;
  static uint32_t chg = 0, down = 0;
  uint32_t now = millis();
  bool r = digitalRead(BTN_PIN);
  if (r != raw) { raw = r; chg = now; }
  if ((uint32_t)(now - chg) >= DEB_MS && stable != raw) {
    stable = raw;
    if (stable == LOW) { down = now; longDone = false; }
    else if (!longDone) btnShort = true;
  }
  if (stable == LOW && !longDone && (uint32_t)(now - down) >= LONG_MS) {
    longDone = true; btnLong = true;
  }
}

// Wait that keeps serial + button serviced.
void pause(uint16_t ms) {
  uint32_t t = millis();
  do { pollSerial(); pollBtn(); } while ((uint32_t)(millis() - t) < ms);
}

// ───────────────────────── LCD helpers ─────────────────────────
void uploadChars() {
  uint8_t b[8];
  for (uint8_t c = 0; c < 8; c++) {
    for (uint8_t i = 0; i < 8; i++) b[i] = pgm_read_byte(&CC[c][i]);
    lcd.createChar(c, b);
  }
}

// Writes only the cells that changed.
void flush() {
  for (uint8_t r = 0; r < 2; r++) {
    bool at = false;
    for (uint8_t c = 0; c < 16; c++) {
      if (fb[r][c] != cur[r][c]) {
        if (!at) lcd.setCursor(c, r);
        lcd.write(fb[r][c]);
        cur[r][c] = fb[r][c];
        at = true;
      } else at = false;
    }
    pollSerial();
  }
}

void fbClr() { memset(fb, ' ', 32); }

void loadP(uint8_t* d, const char* p) {
  uint8_t i = 0;
  while (i < 16) { uint8_t c = pgm_read_byte(p + i); if (!c) break; d[i++] = c; }
  while (i < 16) d[i++] = ' ';
}
void fbTxtP(uint8_t r, const char* p) { loadP(fb[r], p); }

void typeBuf(uint8_t r, const uint8_t* b, uint16_t ms) {
  for (uint8_t i = 0; i < 16; i++) {
    fb[r][i] = b[i]; flush();
    if (b[i] != ' ') pause(ms);
  }
}
void typeP(uint8_t r, const char* p, uint16_t ms) {
  uint8_t b[16]; loadP(b, p); typeBuf(r, b, ms);
}

void flash(uint8_t n) {
  while (n--) { lcd.noDisplay(); pause(90); lcd.display(); pause(105); }
}

void wipeL(uint16_t ms) {
  for (uint8_t c = 0; c < 16; c++) { fb[0][c] = ' '; fb[1][c] = ' '; flush(); pause(ms); }
}
void wipeR(uint16_t ms) {
  for (int8_t c = 15; c >= 0; c--) { fb[0][c] = ' '; fb[1][c] = ' '; flush(); pause(ms); }
}

void slideBuf(const uint8_t t[2][16], uint16_t ms) {
  for (uint8_t w = 1; w <= 16; w++) {
    for (uint8_t c = 0; c < 16; c++) {
      fb[0][c] = (c < w) ? t[0][c] : ' ';
      fb[1][c] = (c < w) ? t[1][c] : ' ';
    }
    flush(); pause(ms);
  }
}

void scrollP(uint8_t r, const char* p, uint16_t ms) {
  int16_t L = (int16_t)strlen_P(p);
  for (int16_t i = 0; i <= L + 16; i++) {
    for (uint8_t c = 0; c < 16; c++) {
      int16_t mi = i + c - 16;
      fb[r][c] = (mi >= 0 && mi < L) ? pgm_read_byte(p + mi) : ' ';
    }
    flush(); pause(ms);
  }
}

// ─────────────────────────── home screen ───────────────────────────
void homeBuf(uint8_t t[2][16]) {
  if (sleeping)      { loadP(t[0], S_SD0);   loadP(t[1], S_SD1); }
  else if (session)  { loadP(t[0], S_SES0);  loadP(t[1], S_SES1); }
  else               { loadP(t[0], S_HOME0); loadP(t[1], S_HOME1); }
}

void showHome(bool slide) {
  uint8_t t[2][16];
  homeBuf(t);
  if (slide) slideBuf(t, 16); else { memcpy(fb, t, 32); flush(); }
  homeDirty = false;
}

void sceneBegin() { lcd.display(); blinkOn = true; idleOn = false; }

// ───────────────────────────── scenes ─────────────────────────────
void sceneBoot() {
  sceneBegin();
  fbClr(); flush();
  for (uint8_t c = 0; c < 16; c++) { fb[0][c] = 6; fb[1][15 - c] = 6; flush(); pause(16); }
  pause(80);
  for (uint8_t s = 0; s < 8; s++) {
    fb[0][s] = ' '; fb[0][15 - s] = ' '; fb[1][s] = ' '; fb[1][15 - s] = ' ';
    flush(); pause(30);
  }
  pause(60);
  static const uint8_t sp0[4] = {1, 5, 9, 13}, sp1[4] = {2, 6, 10, 14};
  for (uint8_t k = 0; k < 4; k++) { fb[0][sp0[k]] = 1; flush(); pause(60); }
  for (uint8_t k = 0; k < 4; k++) { fb[1][sp1[k]] = 1; flush(); pause(60); }
  pause(100);
  fbClr(); flush();
  typeP(0, S_HOME0, 44); pause(60);
  typeP(1, S_BOOT1, 36); pause(80);
  for (uint8_t i = 0; i < 16; i++) { fb[1][i] = 6; flush(); pause(40); }
  pause(100); flash(2); pause(100);
}

void sceneWifi(uint8_t st) {
  sceneBegin();
  if (st == 0) {
    if (wifiSt != 0) return;                       // already resolved while queued
    fbClr(); fbTxtP(0, S_WSET); flush();
    uint32_t t0 = millis(); uint8_t k = 0;
    while (wifiSt == 0 && (uint32_t)(millis() - t0) < 40000UL) {
      memset(fb[1], ' ', 16);
      for (uint8_t i = 0; i < 12; i++) fb[1][i] = pgm_read_byte(S_CONN + i);
      for (uint8_t d = 0; d < k; d++) fb[1][12 + d] = '.';
      flush(); pause(480);
      k = (k + 1) & 3;
    }
    if (wifiSt == 0) wifiSt = 255;                 // ESP went silent
    if (!(evN && evq[0].t == E_WIFI)) showHome(true);
  } else if (st == 1 || st == 2) {
    typeP(0, st == 1 ? S_WON0 : S_WOF0, 40);
    typeP(1, st == 1 ? S_WON1 : S_WOF1, 40);
    flash(st == 1 ? 1 : 2);
    pause(5000);
    showHome(true);
  }
}

void sceneLED(uint8_t n, uint8_t on) {
  if (n > 4) return;
  sceneBegin();
  uint8_t nm[16], tg[16];
  loadP(nm, (const char*)pgm_read_word(&NAMES[n]));
  loadP(tg, (const char*)pgm_read_word(on ? &TON[n] : &TOFF[n]));
  if (on) {
    fb[0][0] = 1; fb[0][15] = 1; fb[1][0] = 1; fb[1][15] = 1; flush(); pause(80);
    for (uint8_t w = 1; w <= 16; w++) {
      for (uint8_t c = 0; c < 16; c++) {
        fb[0][c] = (c < w) ? nm[c] : ' ';
        fb[1][c] = (c < w) ? tg[c] : ' ';
      }
      flush(); pause(32);
    }
    flash(1);
  } else {
    memcpy(fb[0], nm, 16); flush();
    for (uint8_t i = 0; i < 16; i++) { fb[1][i] = '_'; flush(); pause(16); }
    pause(60);
    memcpy(fb[1], tg, 16); flush();
  }
  homeDirty = true;
}

void sceneSessOn() {
  sceneBegin();
  session = true; sleeping = false;
  for (uint8_t pass = 0; pass < 3; pass++)
    for (uint8_t p = 0; p < 16; p++) {
      fbClr(); fb[0][p] = 1; fb[1][15 - p] = 1; flush(); pause(34);
    }
  for (uint8_t c = 0; c < 16; c++) { fb[0][c] = 0; fb[1][c] = 0; flush(); pause(20); }
  pause(80);
  fbClr(); flush();
  typeP(0, S_SES0, 40);
  typeP(1, S_SES1, 40);
  flash(3);
  homeDirty = false;
  sA = 0; sB = 15; sAd = true; sBd = false; noteT = millis();
}

void sceneSessOff() {
  sceneBegin();
  session = false;
  fbClr(); flush();
  scrollP(0, S_SEND, 68);
  typeP(0, S_SOFF0, 40);
  typeP(1, S_SOFF1, 40);
  pause(700);
  showHome(true);
}

void sceneShut() {
  sceneBegin();
  session = false; sleeping = true;
  seenMask = 0; ledMask = 0; memset(lcnt, 0, sizeof(lcnt));       // hard data reset
  ip4[0] = ip4[1] = ip4[2] = ip4[3] = 0;
  fbClr(); flush();                                              // no toggle text before/after
  scrollP(0, S_SHDN, 58);
  for (uint8_t g = 0; g < 14; g++) {
    uint8_t m = g & 3, sym = (m == 0) ? 0 : (m == 1) ? 1 : (m == 2) ? 6 : 7;
    for (uint8_t c = 0; c < 16; c++) {
      fb[0][c] = (((c + g) % 3) == 0) ? sym : ' ';
      fb[1][c] = (((c + g + 1) % 3) == 0) ? sym : ' ';
    }
    flush(); pause(50);
  }
  for (uint8_t s = 0; s < 8; s++) {
    fb[0][s] = ' '; fb[0][15 - s] = ' '; fb[1][s] = ' '; fb[1][15 - s] = ' ';
    flush(); pause(48);
  }
  fbClr(); flush(); pause(100);
  for (uint8_t i = 0; i < 8; i++) {
    fb[0][7 - i] = 0; fb[0][8 + i] = 0; fb[1][7 - i] = 0; fb[1][8 + i] = 0;
    flush(); pause(34);
  }
  pause(80);
  wipeL(18);
  showHome(false);
  for (uint8_t i = 0; i < 3; i++) { pause(280); lcd.noDisplay(); pause(130); lcd.display(); }
}

void sceneWake() {
  sceneBegin();
  sleeping = false; session = false;
  sceneBoot();
  showHome(false);
}

void playNext() {
  Ev e = evq[0];
  evDel(0);
  switch (e.t) {
    case E_LED:  sceneLED(e.a & 0x7F, (e.a >> 7) & 1); break;
    case E_SON:  sceneSessOn();  break;
    case E_SOFF: sceneSessOff(); break;
    case E_SHUT: sceneShut();    break;
    case E_WAKE: sceneWake();    break;
    case E_WIFI: sceneWifi(e.a); break;
  }
  uint32_t n = millis();
  lastAct = n; sceneEndT = n;
}

// ──────────────────── log (button tap) and IP (button hold) ────────────────────
uint8_t cntSeen() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < 5; i++) if (seenMask & (1 << i)) n++;
  return n;
}
uint8_t nthSeen(uint8_t k) {
  for (uint8_t i = 0; i < 5; i++)
    if (seenMask & (1 << i)) { if (k == 0) return i; k--; }
  return 255;
}

void showLog() {
  uint8_t slot = nthSeen(logIdx), tot = cntSeen();
  if (slot > 4) return;
  char ctr[8], nm[17], tg[17], cs[6];
  snprintf(ctr, sizeof(ctr), "%u/%u", (unsigned)(logIdx + 1), (unsigned)tot);
  uint8_t cl = strlen(ctr);
  bool on = ledMask & (1 << slot);

  strncpy_P(nm, (const char*)pgm_read_word(&NAMES[slot]), 16); nm[16] = 0;
  strncpy_P(tg, (const char*)pgm_read_word(on ? &TON[slot] : &TOFF[slot]), 16); tg[16] = 0;

  memset(fb[0], ' ', 16);
  uint8_t nl = strlen(nm), mx = 16 - cl - 1;
  if (nl > mx) nl = mx;
  memcpy(fb[0], nm, nl);
  memcpy(fb[0] + 16 - cl, ctr, cl);

  memset(fb[1], ' ', 16);
  uint8_t tl = strlen(tg);
  memcpy(fb[1], tg, tl);
  if (lcnt[slot]) {
    snprintf(cs, sizeof(cs), "x%u", (unsigned)lcnt[slot]);
    uint8_t sl = strlen(cs);
    if (tl + 1 + sl <= 16) memcpy(fb[1] + 16 - sl, cs, sl);
  }
  flush();
}

void enterLog() {
  lcd.display(); blinkOn = true;
  if (!cntSeen()) {
    memcpy(sv, fb, 32);
    fbTxtP(0, S_LOGE0); fbTxtP(1, S_LOGE1); flush();
    pause(1100);
    memcpy(fb, sv, 32); flush();
    return;
  }
  memcpy(sv, fb, 32);
  inLog = true; logIdx = 0; logT = millis();
  showLog();
}

void nextLog() {
  uint8_t tot = cntSeen();
  if (!tot) return;
  logIdx = (logIdx + 1) % tot;
  wipeR(9);
  showLog();
  logT = millis();
}

void exitOverlay() {
  inIP = false; inLog = false;
  lcd.display(); blinkOn = true;
  evKeepLastLED();                         // don't replay a backlog of old toggles
  slideBuf(sv, 16);
  uint32_t n = millis();
  lastAct = n; sceneEndT = n; idleOn = false;
}

void doLongPress() {
  lcd.display(); blinkOn = true; idleOn = false;
  if (!inIP && !inLog) memcpy(sv, fb, 32);
  inIP = false; inLog = false;

  const char *a, *b;
  if      (wifiSt == 1)   { a = S_LWON0; b = S_WON1;  }
  else if (wifiSt == 0)   { a = S_LWLK0; b = S_LWLK1; }
  else if (wifiSt == 2)   { a = S_LWOF0; b = S_WOF1;  }
  else                    { a = S_LWUN0; b = S_LWUN1; }
  wipeR(13);
  typeP(0, a, 38); typeP(1, b, 38);
  pause(2800);
  wipeL(13);

  typeP(0, S_IP0, 40);
  char s[17]; uint8_t line[16];
  memset(line, ' ', 16);
  if (!(ip4[0] | ip4[1] | ip4[2] | ip4[3])) strcpy_P(s, S_NOIP); else ipStr(s);
  uint8_t L = strlen(s), off = (16 - L) / 2;
  memcpy(line + off, s, L);
  typeBuf(1, line, 44);
  flash(2);
  inIP = true; ipT = millis();
}

// ───────────────────────────── idle ─────────────────────────────
void idleRow1() {
  memset(fb[1], ' ', 16);
  switch (idleStyle) {
    case 0:                                         // floating note
      if (notePos < 0) notePos = 0;
      if (notePos > 15) notePos = 15;
      fb[1][notePos] = 5;
      notePos += noteDir;
      if (notePos >= 15) noteDir = -1;
      if (notePos <= 0)  noteDir = 1;
      break;
    case 1:                                         // twinkle
      for (uint8_t k = 0; k < 3; k++) {
        rs = rs * 25173u + 13849u;
        fb[1][(rs >> 8) & 15] = (k == 0) ? 5 : 1;
      }
      break;
    case 2:                                         // dot wave
      for (uint8_t c = 0; c < 16; c++) if (((c + ph) & 3) == 0) fb[1][c] = 7;
      break;
    case 3: {                                       // drifting moon with trail
      uint8_t pos = ph % 20;
      if (pos >= 1 && pos - 1 < 16) fb[1][pos - 1] = 7;
      if (pos >= 2 && pos - 2 < 16) fb[1][pos - 2] = 7;
      if (pos < 16) fb[1][pos] = 2;
      break;
    }
  }
  ph++;
}

void idleTick() {
  uint32_t now = millis();

  if (sleeping) {                                   // slow blink
    if ((uint32_t)(now - idleT) >= 1000UL) {
      idleT = now; blinkOn = !blinkOn;
      if (blinkOn) lcd.display(); else lcd.noDisplay();
    }
    return;
  }

  if (session) {                                    // note + spark bounce
    if ((uint32_t)(now - lastAct) < SESS_IDLE_MS) return;
    if (homeDirty) { showHome(true); return; }
    if ((uint32_t)(now - noteT) < 440UL) return;
    noteT = now;
    memset(fb[1], ' ', 16);
    fb[1][(uint8_t)sA] = 5; fb[1][(uint8_t)sB] = 1;
    flush();
    if (sAd) sA++; else sA--;
    if (sBd) sB++; else sB--;
    if (sA >= 14) sAd = false;
    if (sA <= 1)  sAd = true;
    if (sB >= 14) sBd = false;
    if (sB <= 1)  sBd = true;
    return;
  }

  if ((uint32_t)(now - lastAct) < IDLE_MS) return;
  if (!idleOn) { idleOn = true; idleT = now - 500UL; noteT = now - 800UL; idleTk = 0; }

  if ((uint32_t)(now - idleT) >= 500UL) {
    idleT = now;
    if (++idleTk >= 8) {
      idleTk = 0;
      idleMsg = (idleMsg + 1) % IMSG_N;
      if ((idleMsg & 7) == 0) idleStyle = (idleStyle + 1) & 3;
      memset(fb[0], ' ', 16); flush(); pause(30);
    }
    fbTxtP(0, (const char*)pgm_read_word(&IMSGS[idleMsg]));
    flush();
  }
  if ((uint32_t)(now - noteT) >= 700UL) {
    noteT = now;
    idleRow1();
    flush();
  }
}

// ───────────────────────── silent maintenance ─────────────────────────
// Runs every 10 min while idle. Touches data only; the screen content never changes.
void maintenance() {
  uint32_t now = millis();
  maintT = now;
  memset(lcnt, 0, sizeof(lcnt));                          // old log history
  if ((uint32_t)(now - lastSync) > MAINT_MS) seenMask = 0; // ESP silent -> cache is stale
  rn = 0;                                                 // parser scratch
  uploadChars();                                          // re-arm custom chars
  memset(cur, 0xFF, sizeof(cur));                         // rewrite same text -> heals LCD noise
  flush();
}

// Apply ESP truth silently (no animation) when nothing is pending.
void reconcile() {
  syncNew = false;
  if (rxWifi <= 2 && rxWifi != wifiSt) wifiSt = rxWifi;
  bool wantD = (rxMode == 2), wantS = (rxMode == 1);
  if (wantD != sleeping || (wantS && !wantD) != session) {
    sleeping = wantD; session = wantS && !wantD;
    showHome(false);
  }
}

// ─────────────────────────── setup / loop ───────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  lcd.begin(16, 2);
  uploadChars();
  lcd.clear();
  memset(cur, ' ', sizeof(cur));
  fbClr();
  Serial.println(F("[desk] display unit v2 ready"));
  sceneBoot();
  showHome(false);
  uint32_t n = millis();
  lastAct = n; sceneEndT = n; maintT = n; idleT = n; noteT = n;
}

void loop() {
  pollSerial();
  pollBtn();
  uint32_t now = millis();

  if (btnLong)  { btnLong = false; btnShort = false; doLongPress(); return; }
  if (btnShort) {
    btnShort = false;
    if (inIP) exitOverlay(); else if (inLog) nextLog(); else enterLog();
    return;
  }
  if (inIP  && (uint32_t)(now - ipT)  >= IP_MS)  { exitOverlay(); return; }
  if (inLog && (uint32_t)(now - logT) >= LOG_MS) { exitOverlay(); return; }

  if (!inIP && !inLog) {
    if (evN) {
      if (evq[0].t == E_WIFI || (uint32_t)(now - sceneEndT) >= GAP_MS) playNext();
    } else {
      if (syncNew) reconcile();
      idleTick();
      if ((uint32_t)(now - maintT) >= MAINT_MS) maintenance();
    }
  }
}
