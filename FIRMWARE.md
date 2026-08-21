# flock-mini

Passive Flock Safety camera detector for the **HW-364A** (ESP8266 + onboard 0.96" SSD1306 OLED).

Receive-only: it never transmits, never deauthenticates, never associates. It puts the radio in
promiscuous mode, hops channels 1/6/11, and matches 802.11 frames against the Flock Safety
signature set published by the community.

Credits: OUI research by **@NitekryDPaul**, the 31st prefix and the wildcard-probe signature by
**Michael / DeFlockJoplin**, original firmware **[colonelpanichacks/flock-you](https://github.com/colonelpanichacks/flock-you)**,
prior ESP8266 port **[LuxStatera/flock-hunter-d1-mini-wifi](https://github.com/LuxStatera/flock-hunter-d1-mini-wifi)**.

## Detection tiers

| Tier | Signature | Alerts? |
|---|---|---|
| HIGH | Probe Request with a zero-length SSID element (wildcard probe) from a known Flock OUI | Yes - screen + LED + buzzer |
| HIGH | Beacon / Probe Response whose SSID contains `flock`, `flck`, `fs ext battery`, `penguin`, `pigvision` | Yes |
| LOW | OUI-only match on transmitter, receiver, or BSSID address | Recorded silently, shown in the list |

The low tier is deliberately quiet: `a4:cf:12`, `3c:71:bf` and `08:3a:88` in the community list are
Espressif prefixes and `e4:aa:ea` is Liteon, so an OUI-only hit fires on ordinary IoT gear too.

## Files

Create a folder named exactly `flock-mini`, and put the two source files in it. Arduino IDE requires
the folder name to match the `.ino` name.

```
flock-mini/
  flock-mini.ino
  flock_sigs.h
  platformio.ini   (only if you use PlatformIO)
```

---

## File 1: `flock_sigs.h`

```cpp
// flock_sigs.h - Flock Safety WiFi signatures.
//
// 30 prefixes from @NitekryDPaul's promiscuous-mode research, 82:6b:f2 added by
// Michael / DeFlockJoplin after drive-testing in Joplin, b4:1e:52 is the OUI
// registered to Flock Safety itself.
//
// Several of these belong to contract manufacturers rather than to Flock:
// a4:cf:12, 3c:71:bf and 08:3a:88 are Espressif, e4:aa:ea is Liteon. A bare OUI
// match on those is weak evidence, which is why the firmware ranks it low.

#pragma once
#include <stdint.h>

static const uint8_t FLOCK_OUIS[][3] = {
  {0x70,0xc9,0x4e}, {0x3c,0x91,0x80}, {0xd8,0xf3,0xbc}, {0x80,0x30,0x49},
  {0xb8,0x35,0x32}, {0x14,0x5a,0xfc}, {0x74,0x4c,0xa1}, {0x08,0x3a,0x88},
  {0x9c,0x2f,0x9d}, {0xc0,0x35,0x32}, {0x94,0x08,0x53}, {0xe4,0xaa,0xea},
  {0xf4,0x6a,0xdd}, {0xf8,0xa2,0xd6}, {0x24,0xb2,0xb9}, {0x00,0xf4,0x8d},
  {0xd0,0x39,0x57}, {0xe8,0xd0,0xfc}, {0xe0,0x4f,0x43}, {0xb8,0x1e,0xa4},
  {0x70,0x08,0x94}, {0x58,0x8e,0x81}, {0xec,0x1b,0xbd}, {0x3c,0x71,0xbf},
  {0x58,0x00,0xe3}, {0x90,0x35,0xea}, {0x5c,0x93,0xa2}, {0x64,0x6e,0x69},
  {0x48,0x27,0xea}, {0xa4,0xcf,0x12}, {0x82,0x6b,0xf2}, {0xb4,0x1e,0x52},
};

static const uint8_t FLOCK_OUI_COUNT = sizeof(FLOCK_OUIS) / 3;

// Lowercase; SSIDs are folded to lowercase before comparison.
static const char* const FLOCK_SSID_KEYWORDS[] = {
  "flock", "flck", "fs ext battery", "penguin", "pigvision",
};

static const uint8_t FLOCK_SSID_KEYWORD_COUNT =
    sizeof(FLOCK_SSID_KEYWORDS) / sizeof(FLOCK_SSID_KEYWORDS[0]);

// ---------------------------------------------------------------- shared types
//
// These live here rather than in the .ino because the Arduino build generates
// function prototypes at the very top of the sketch. A prototype for
// handleHit(const Hit&) would otherwise be emitted before Hit exists.

enum Method : uint8_t {
  M_WILDCARD_PROBE = 0,
  M_SSID_KEYWORD,
  M_OUI_TX,
  M_OUI_RX,
  M_OUI_BSSID,
};

struct Hit {
  uint8_t  mac[6];
  int8_t   rssi;
  uint8_t  ch;
  uint8_t  method;
  char     ssid[20];
};

struct Det {
  uint8_t  mac[6];
  int8_t   rssi;
  uint8_t  ch;
  uint8_t  method;        // best (lowest-ranked) method seen for this MAC
  uint16_t hits;
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint32_t lastPrinted;
  char     ssid[20];
};

// The ESP8266 Arduino core does not export the SDK's RxControl, so mirror it.
// This is the 12-byte metadata header in front of every promiscuous frame.
struct FMRxControl {
  signed   rssi:          8;
  unsigned rate:          4;
  unsigned is_group:      1;
  unsigned:               1;
  unsigned sig_mode:      2;
  unsigned legacy_length: 12;
  unsigned damatch0:      1;
  unsigned damatch1:      1;
  unsigned bssidmatch0:   1;
  unsigned bssidmatch1:   1;
  unsigned MCS:           7;
  unsigned CWB:           1;
  unsigned HT_length:     16;
  unsigned Smoothing:     1;
  unsigned Not_Sounding:  1;
  unsigned:               1;
  unsigned Aggregation:   1;
  unsigned STBC:          2;
  unsigned FEC_CODING:    1;
  unsigned SGI:           1;
  unsigned rxend_state:   8;
  unsigned ampdu_cnt:     8;
  unsigned channel:       4;
  unsigned:               12;
};

static_assert(sizeof(struct FMRxControl) == 12, "RxControl must be 12 bytes");
```

---

## File 2: `flock-mini.ino`

```cpp
// flock-mini - passive Flock Safety camera detector
// HW-364A / HW-364B: ESP8266 + onboard 0.96" SSD1306 (I2C 0x3C, SDA=GPIO14, SCL=GPIO12)
//
// Passive receiver only. Requires the U8g2 library (Library Manager -> "U8g2" by oliver).

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>

#include "flock_sigs.h"

extern "C" {
#include "user_interface.h"
}

// ---------------------------------------------------------------- build options

#define OLED_SDA        14      // HW-364A: SDA is GPIO14, silkscreened D6
#define OLED_SCL        12      // HW-364A: SCL is GPIO12, silkscreened D5
#define OLED_ADDR       0x3C

#define USE_LED         1
#define LED_PIN         2       // onboard LED, active low
#define USE_BUZZER      0       // passive piezo on GPIO5 if you add one
#define BUZZER_PIN      5
#define USE_BUTTON      1
#define BUTTON_PIN      0       // FLASH button: tap = next screen, hold = mute

#define FULL_HOP        0       // 1 = sweep channels 1-13 instead of 1/6/11
#define CHANNEL_DWELL   350     // ms; cameras spray wildcard probes every ~125ms
#define RSSI_FLOOR      -95

#define MAX_DETECTIONS  64
#define HIT_QUEUE       12

#define ALERT_HOLD_MS   4000
#define LIST_HOLD_MS    5000
#define ACTIVE_MS       10000   // "in range" window
#define REDISCOVER_MS   30000   // re-alert on a camera seen again after this
#define SERIAL_DEDUP_MS 5000
#define SCREEN_MS       400
#define STATUS_MS       30000

// Self-test: uncomment and set to your phone's WiFi OUI. Phones emit wildcard
// probe requests constantly, so this exercises the exact high-confidence path a
// real camera would trip.
// #define SELFTEST_OUI 0xA1B2C3

// ---------------------------------------------------------------- detection model

// Method, Hit, Det and FMRxControl are defined in flock_sigs.h - see the note there.

static bool isHighConfidence(uint8_t m) { return m <= M_SSID_KEYWORD; }

static const char* methodJson(uint8_t m) {
  switch (m) {
    case M_WILDCARD_PROBE: return "wifi_wildcard_probe";
    case M_SSID_KEYWORD:   return "wifi_ssid";
    case M_OUI_TX:         return "wifi_oui_addr2";
    case M_OUI_RX:         return "wifi_oui_addr1";
    default:               return "wifi_oui_addr3";
  }
}

static const char* methodShort(uint8_t m) {
  switch (m) {
    case M_WILDCARD_PROBE: return "WPROBE";
    case M_SSID_KEYWORD:   return "SSID";
    case M_OUI_TX:         return "OUI-TX";
    case M_OUI_RX:         return "OUI-RX";
    default:               return "OUI-BSS";
  }
}

static Hit hitQueue[HIT_QUEUE];
static volatile uint8_t qHead = 0, qTail = 0;
static volatile uint16_t qDropped = 0;

static Det dets[MAX_DETECTIONS];
static uint8_t detCount = 0;

// Targets = the shipped OUI list plus the optional self-test prefix.
static uint8_t targets[FLOCK_OUI_COUNT + 1][3];
static uint8_t targetCount = 0;
static uint32_t firstByteMask[8];

// ---------------------------------------------------------------- display

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

#define UI_SCAN  0
#define UI_ALERT 1
#define UI_LIST  2

static uint8_t uiMode = UI_SCAN;
static uint32_t uiSince = 0;
static uint8_t listPage = 0;
static int8_t alertIdx = -1;
static bool muted = false;
static bool manualUi = false;

static uint8_t currentChannel = 1;
static uint32_t bootAt = 0, lastHop = 0, lastDraw = 0, lastStatus = 0, ledOffAt = 0;
static uint8_t dotPhase = 0;

static const uint8_t hopSet[] =
#if FULL_HOP
    {1,2,3,4,5,6,7,8,9,10,11,12,13};
#else
    {1, 6, 11};
#endif
static const uint8_t hopCount = sizeof(hopSet);
static uint8_t hopIdx = 0;

// ---------------------------------------------------------------- matching

static void buildTargets() {
  for (uint8_t i = 0; i < FLOCK_OUI_COUNT; i++) {
    memcpy(targets[targetCount], FLOCK_OUIS[i], 3);
    targetCount++;
  }
#ifdef SELFTEST_OUI
  targets[targetCount][0] = (SELFTEST_OUI >> 16) & 0xff;
  targets[targetCount][1] = (SELFTEST_OUI >> 8) & 0xff;
  targets[targetCount][2] = (SELFTEST_OUI) & 0xff;
  targetCount++;
#endif
  memset(firstByteMask, 0, sizeof(firstByteMask));
  for (uint8_t i = 0; i < targetCount; i++) {
    uint8_t b = targets[i][0];
    firstByteMask[b >> 5] |= (1UL << (b & 31));
  }
}

static inline bool firstBytePossible(uint8_t b) {
  return firstByteMask[b >> 5] & (1UL << (b & 31));
}

// No locally-administered filter here on purpose: 82:6b:f2 has that bit set and
// is a confirmed camera prefix, so filtering it would drop a real signature.
static bool matchTarget(const uint8_t* mac) {
  if (!firstBytePossible(mac[0])) return false;
  for (uint8_t i = 0; i < targetCount; i++) {
    if (mac[0] == targets[i][0] && mac[1] == targets[i][1] && mac[2] == targets[i][2])
      return true;
  }
  return false;
}

static bool findSsidIe(const uint8_t* ies, int len, const uint8_t** out, uint8_t* outLen) {
  while (len >= 2) {
    uint8_t id = ies[0], elen = ies[1];
    if (2 + (int)elen > len) return false;
    if (id == 0) { *out = ies + 2; *outLen = elen; return true; }
    ies += 2 + elen;
    len -= 2 + elen;
  }
  return false;
}

static bool ssidHasKeyword(const uint8_t* ssid, uint8_t len) {
  char low[33];
  if (len > 32) len = 32;
  for (uint8_t i = 0; i < len; i++) {
    char c = (char)ssid[i];
    low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  low[len] = '\0';
  for (uint8_t k = 0; k < FLOCK_SSID_KEYWORD_COUNT; k++)
    if (strstr(low, FLOCK_SSID_KEYWORDS[k])) return true;
  return false;
}

static void pushHit(uint8_t method, const uint8_t* mac, int8_t rssi, uint8_t ch,
                    const uint8_t* ssid, uint8_t ssidLen) {
  uint8_t next = (uint8_t)((qHead + 1) % HIT_QUEUE);
  if (next == qTail) { qDropped++; return; }
  Hit* h = &hitQueue[qHead];
  memcpy(h->mac, mac, 6);
  h->rssi = rssi;
  h->ch = ch;
  h->method = method;
  uint8_t n = (ssid == NULL) ? 0 : ssidLen;
  if (n > sizeof(h->ssid) - 1) n = sizeof(h->ssid) - 1;
  if (n) memcpy(h->ssid, ssid, n);
  h->ssid[n] = '\0';
  qHead = next;
}

// ---------------------------------------------------------------- sniffer
//
// ESP8266 SDK buffer shapes, which are easy to get backwards:
//   len == 12  -> struct RxControl only, no frame bytes at all
//   len == 128 -> struct sniffer_buf2: RxControl + up to 112 bytes of a
//                 MANAGEMENT frame, i.e. header AND body. This is the only case
//                 where we can read information elements.
//   otherwise  -> struct sniffer_buf: RxControl + 36 bytes of a DATA frame,
//                 header addresses only, no body.

static void sniffCb(uint8_t* buf, uint16_t len) {
  if (len < 24) return;

  const FMRxControl* rx = (const FMRxControl*)buf;
  int8_t rssi = (int8_t)rx->rssi;
  if (rssi < RSSI_FLOOR) return;
  uint8_t ch = rx->channel;
  if (ch < 1 || ch > 14) ch = currentChannel;

  const uint8_t* frame = buf + sizeof(FMRxControl);
  int avail;
  if (len == 128) {
    uint16_t real = (uint16_t)buf[124] | ((uint16_t)buf[125] << 8);
    avail = (real >= 24 && real < 112) ? (int)real : 112;
  } else {
    avail = (int)len - (int)sizeof(FMRxControl);
    if (avail > 36) avail = 36;
  }
  if (avail < 24) return;

  const uint8_t fc0 = frame[0];
  const uint8_t ftype = (fc0 >> 2) & 0x03;
  const uint8_t subtype = (fc0 >> 4) & 0x0f;
  const uint8_t* a1 = frame + 4;
  const uint8_t* a2 = frame + 10;
  const uint8_t* a3 = frame + 16;

  if (ftype == 0 && avail > 24) {
    int ieOff = -1;
    if (subtype == 4) ieOff = 24;                       // probe request
    else if (subtype == 8 || subtype == 5) ieOff = 36;   // beacon / probe response
    if (ieOff > 0 && avail > ieOff) {
      const uint8_t* ssid = NULL;
      uint8_t ssidLen = 0;
      if (findSsidIe(frame + ieOff, avail - ieOff, &ssid, &ssidLen)) {
        if (subtype == 4 && ssidLen == 0 && matchTarget(a2)) {
          pushHit(M_WILDCARD_PROBE, a2, rssi, ch, NULL, 0);
          return;
        }
        if (ssidLen > 0 && ssidHasKeyword(ssid, ssidLen)) {
          pushHit(M_SSID_KEYWORD, a2, rssi, ch, ssid, ssidLen);
          return;
        }
      }
    }
  }

  if (matchTarget(a2)) { pushHit(M_OUI_TX, a2, rssi, ch, NULL, 0); return; }

  // addr1 is the receiver: catches cameras that are asleep and only being
  // addressed by a nearby AP (the @NitekryDPaul technique).
  if (!(a1[0] & 0x01) && matchTarget(a1)) { pushHit(M_OUI_RX, a1, rssi, ch, NULL, 0); return; }

  if (ftype == 0 && !(a3[0] & 0x01) && matchTarget(a3))
    pushHit(M_OUI_BSSID, a3, rssi, ch, NULL, 0);
}

// ---------------------------------------------------------------- alerts

static void alertLed() {
#if USE_LED
  digitalWrite(LED_PIN, LOW);
  ledOffAt = millis() + 250;
#endif
}

static void alertBuzz() {
#if USE_BUZZER
  if (muted) return;
  tone(BUZZER_PIN, 2000, 60);
#endif
}

static void ledTick() {
#if USE_LED
  if (ledOffAt && millis() >= ledOffAt) {
    digitalWrite(LED_PIN, HIGH);
    ledOffAt = 0;
  }
#endif
}

// ---------------------------------------------------------------- table

static int findDet(const uint8_t* mac) {
  for (uint8_t i = 0; i < detCount; i++)
    if (memcmp(dets[i].mac, mac, 6) == 0) return i;
  return -1;
}

static void handleHit(const Hit& h) {
  uint32_t now = millis();
  int idx = findDet(h.mac);
  bool worthAlert = false;

  if (idx < 0) {
    if (detCount >= MAX_DETECTIONS) return;
    idx = detCount++;
    Det& d = dets[idx];
    memcpy(d.mac, h.mac, 6);
    d.method = h.method;
    d.hits = 1;
    d.firstSeen = now;
    d.lastPrinted = 0;
    strncpy(d.ssid, h.ssid, sizeof(d.ssid) - 1);
    d.ssid[sizeof(d.ssid) - 1] = '\0';
    worthAlert = true;
  } else {
    Det& d = dets[idx];
    if (d.hits < 0xFFFF) d.hits++;
    if (now - d.lastSeen > REDISCOVER_MS) worthAlert = true;
    if (h.method < d.method) {           // upgraded to a stronger signature
      d.method = h.method;
      worthAlert = true;
    }
    if (h.ssid[0] && !d.ssid[0]) {
      strncpy(d.ssid, h.ssid, sizeof(d.ssid) - 1);
      d.ssid[sizeof(d.ssid) - 1] = '\0';
    }
  }

  Det& d = dets[idx];
  d.rssi = h.rssi;
  d.ch = h.ch;
  d.lastSeen = now;

  if (d.lastPrinted == 0 || now - d.lastPrinted > SERIAL_DEDUP_MS) {
    d.lastPrinted = now;
    Serial.printf(
      "{\"event\":\"detection\",\"detection_method\":\"%s\",\"confidence\":\"%s\","
      "\"protocol\":\"wifi_2_4ghz\",\"mac_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
      "\"oui\":\"%02x:%02x:%02x\",\"rssi\":%d,\"channel\":%u,\"frequency\":%u,"
      "\"ssid\":\"%s\",\"hits\":%u}\n",
      methodJson(h.method), isHighConfidence(h.method) ? "high" : "low",
      h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5],
      h.mac[0], h.mac[1], h.mac[2],
      h.rssi, (unsigned)h.ch, (unsigned)(2412 + (h.ch - 1) * 5), d.ssid, (unsigned)d.hits);
  }

  if (worthAlert && isHighConfidence(h.method)) {
    alertIdx = (int8_t)idx;
    uiMode = UI_ALERT;
    uiSince = now;
    manualUi = false;
    alertLed();
    alertBuzz();
  }
}

static void drainHits() {
  while (qTail != qHead) {
    Hit h = hitQueue[qTail];
    qTail = (uint8_t)((qTail + 1) % HIT_QUEUE);
    handleHit(h);
  }
}

static uint8_t countActive() {
  uint8_t n = 0;
  uint32_t now = millis();
  for (uint8_t i = 0; i < detCount; i++)
    if (now - dets[i].lastSeen < ACTIVE_MS) n++;
  return n;
}

static uint8_t countHigh() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < detCount; i++)
    if (isHighConfidence(dets[i].method)) n++;
  return n;
}

// ---------------------------------------------------------------- UI

static const char* rangeLabel(int8_t rssi) {
  if (rssi > -60) return "CLOSE";
  if (rssi > -75) return "NEAR";
  return "FAR";
}

static void fmtUptime(char* buf, size_t cap) {
  uint32_t s = (millis() - bootAt) / 1000;
  if (s < 3600) snprintf(buf, cap, "%lum%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  else snprintf(buf, cap, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60));
}

static void drawFooter() {
  char up[10];
  fmtUptime(up, sizeof(up));
  oled.drawStr(2, 63, up);

  char heap[12];
  snprintf(heap, sizeof(heap), "%uk", (unsigned)(ESP.getFreeHeap() / 1024));
  oled.drawStr(44, 63, heap);

  const char* flag = muted ? "MUTE" : (qDropped ? "DROP" : "RX");
  int w = oled.getStrWidth(flag);
  oled.drawStr(128 - w - 2, 63, flag);
}

static void drawScan() {
  oled.drawStr(2, 7, "FLOCK-MINI");
  char ch[10];
  snprintf(ch, sizeof(ch), "CH:%u", (unsigned)currentChannel);
  int w = oled.getStrWidth(ch);
  oled.drawStr(128 - w - 2, 7, ch);
  oled.drawHLine(2, 9, 124);

  dotPhase = (dotPhase + 1) & 3;
  char msg[20];
  snprintf(msg, sizeof(msg), "Scanning%.*s", dotPhase, "...");
  w = oled.getStrWidth(msg);
  oled.drawStr((128 - w) / 2, 25, msg);

  char line[24];
  uint8_t active = countActive();
  if (active) snprintf(line, sizeof(line), "%u in range", active);
  else snprintf(line, sizeof(line), "nothing in range");
  w = oled.getStrWidth(line);
  oled.drawStr((128 - w) / 2, 37, line);

  snprintf(line, sizeof(line), "high:%u  low:%u", countHigh(), (unsigned)(detCount - countHigh()));
  w = oled.getStrWidth(line);
  oled.drawStr((128 - w) / 2, 48, line);

  oled.drawHLine(2, 53, 124);
  drawFooter();
}

static void drawAlert() {
  if (alertIdx < 0 || alertIdx >= (int)detCount) { drawScan(); return; }
  Det& d = dets[alertIdx];

  oled.drawBox(0, 0, 128, 10);
  oled.setDrawColor(0);
  const char* t = "FLOCK DETECTED";
  int w = oled.getStrWidth(t);
  oled.drawStr((128 - w) / 2, 7, t);
  oled.setDrawColor(1);

  char line[32];
  snprintf(line, sizeof(line), "%02x:%02x:%02x:%02x:%02x:%02x",
           d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
  oled.drawStr(2, 20, line);

  snprintf(line, sizeof(line), "%s  %ddBm", methodShort(d.method), d.rssi);
  oled.drawStr(2, 31, line);

  snprintf(line, sizeof(line), "%s  ch%u  x%u", rangeLabel(d.rssi), (unsigned)d.ch,
           (unsigned)d.hits);
  oled.drawStr(2, 42, line);

  if (d.ssid[0]) {
    snprintf(line, sizeof(line), "\"%s\"", d.ssid);
    oled.drawStr(2, 52, line);
  }

  oled.drawHLine(2, 55, 124);
  drawFooter();
}

static void drawList() {
  char hdr[24];
  uint8_t pages = (detCount + 4) / 5;
  if (pages == 0) pages = 1;
  if (listPage >= pages) listPage = 0;
  snprintf(hdr, sizeof(hdr), "SEEN:%u  pg%u/%u", (unsigned)detCount,
           (unsigned)(listPage + 1), (unsigned)pages);
  oled.drawStr(2, 7, hdr);
  oled.drawHLine(2, 9, 124);

  uint32_t now = millis();
  int y = 19;
  for (uint8_t row = 0; row < 5; row++) {
    int i = detCount - 1 - (listPage * 5 + row);   // newest first
    if (i < 0) break;
    Det& d = dets[i];

    char mac[14];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x", d.mac[0], d.mac[1], d.mac[2], d.mac[3]);
    oled.drawStr(2, y, mac);

    char rssi[6];
    snprintf(rssi, sizeof(rssi), "%d", d.rssi);
    oled.drawStr(62, y, rssi);

    oled.drawStr(86, y, isHighConfidence(d.method) ? "HI" : "lo");
    if (now - d.lastSeen < ACTIVE_MS) oled.drawStr(104, y, "<");

    y += 9;
  }

  oled.drawHLine(2, 53, 124);
  drawFooter();
}

static void draw() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tr);
  oled.setDrawColor(1);
  switch (uiMode) {
    case UI_ALERT: drawAlert(); break;
    case UI_LIST:  drawList();  break;
    default:       drawScan();  break;
  }
  oled.sendBuffer();
}

static void splash() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x14B_tr);
  const char* t = "FLOCK-MINI";
  oled.drawStr((128 - oled.getStrWidth(t)) / 2, 22, t);
  oled.setFont(u8g2_font_5x8_tr);
  const char* s = "passive rx only";
  oled.drawStr((128 - oled.getStrWidth(s)) / 2, 38, s);
  char n[24];
  snprintf(n, sizeof(n), "%u signatures", (unsigned)targetCount);
  oled.drawStr((128 - oled.getStrWidth(n)) / 2, 52, n);
  oled.sendBuffer();
}

static void uiTick() {
  uint32_t now = millis();

  if (!manualUi) {
    if (uiMode == UI_ALERT && now - uiSince > ALERT_HOLD_MS) {
      uiMode = detCount ? UI_LIST : UI_SCAN;
      uiSince = now;
    } else if (uiMode == UI_LIST && now - uiSince > LIST_HOLD_MS) {
      uiMode = UI_SCAN;
      uiSince = now;
    }
  }

  if (now - lastDraw >= SCREEN_MS) {
    lastDraw = now;
    draw();
  }
}

static void buttonTick() {
#if USE_BUTTON
  static bool down = false;
  static uint32_t downAt = 0;
  static bool longFired = false;

  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  uint32_t now = millis();

  if (pressed && !down) {
    down = true;
    downAt = now;
    longFired = false;
  } else if (pressed && down && !longFired && now - downAt > 900) {
    longFired = true;
    muted = !muted;
    Serial.printf("[flock-mini] alerts %s\n", muted ? "muted" : "unmuted");
  } else if (!pressed && down) {
    down = false;
    if (!longFired && now - downAt > 40) {
      manualUi = true;
      uiSince = now;
      if (uiMode == UI_LIST) {
        uint8_t pages = (detCount + 4) / 5;
        if (pages == 0) pages = 1;
        listPage++;
        if (listPage >= pages) { listPage = 0; uiMode = UI_SCAN; manualUi = false; }
      } else {
        uiMode = UI_LIST;
        listPage = 0;
      }
    }
  }
#endif
}

// ---------------------------------------------------------------- radio

static void hopTick() {
  if (hopCount < 2) return;
  if (millis() - lastHop < CHANNEL_DWELL) return;
  hopIdx = (uint8_t)((hopIdx + 1) % hopCount);
  currentChannel = hopSet[hopIdx];
  wifi_set_channel(currentChannel);
  lastHop = millis();
}

static void statusTick() {
  if (millis() - lastStatus < STATUS_MS) return;
  lastStatus = millis();
  Serial.printf("[flock-mini] ch=%u seen=%u high=%u drops=%u heap=%u\n",
                (unsigned)currentChannel, (unsigned)detCount, (unsigned)countHigh(),
                (unsigned)qDropped, ESP.getFreeHeap());
}

// The HW-364 series ships with SDA/SCL swapped on some units, so probe for the
// panel instead of trusting the labels.
static bool probeI2c(uint8_t sda, uint8_t scl) {
  Wire.begin(sda, scl);
  Wire.setClock(400000);
  Wire.beginTransmission(OLED_ADDR);
  return Wire.endTransmission() == 0;
}

static void initDisplay() {
  bool swapped = false, found = true;
  if (!probeI2c(OLED_SDA, OLED_SCL)) {
    if (probeI2c(OLED_SCL, OLED_SDA)) swapped = true;
    else found = false;
  }
  if (swapped) {
    u8x8_SetPin(oled.getU8x8(), U8X8_PIN_I2C_DATA, OLED_SCL);
    u8x8_SetPin(oled.getU8x8(), U8X8_PIN_I2C_CLOCK, OLED_SDA);
  }
  oled.setBusClock(400000);
  oled.begin();
  oled.setFont(u8g2_font_5x8_tr);
  Serial.printf("[flock-mini] oled sda=%u scl=%u%s\n",
                swapped ? OLED_SCL : OLED_SDA, swapped ? OLED_SDA : OLED_SCL,
                swapped ? " (swapped)" : "");
  if (!found)
    Serial.println("[flock-mini] WARNING: no I2C device at 0x3C on either pin order");
}

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println("[flock-mini] boot");

#if USE_LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
#endif
#if USE_BUZZER
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif
#if USE_BUTTON
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif

  buildTargets();
  initDisplay();
  splash();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  wifi_set_opmode(STATION_MODE);
  wifi_set_sleep_type(NONE_SLEEP_T);
  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(sniffCb);
  wifi_promiscuous_enable(1);

  currentChannel = hopSet[0];
  wifi_set_channel(currentChannel);

  bootAt = millis();
  lastHop = bootAt;
  lastStatus = bootAt;
  uiSince = bootAt;
  delay(1200);

  Serial.printf("[flock-mini] sniffing, %u signatures, heap=%u\n",
                (unsigned)targetCount, ESP.getFreeHeap());
}

void loop() {
  hopTick();
  drainHits();
  buttonTick();
  ledTick();
  uiTick();
  statusTick();
  delay(1);
}
```

---

## File 3: `platformio.ini` (optional)

`src_dir = .` is what lets PlatformIO build the same `.ino` that Arduino IDE opens, so there is only
one copy of the source.

```ini
[platformio]
src_dir = .

[env:hw364a]
platform = espressif8266
board = nodemcuv2
framework = arduino
monitor_speed = 115200
upload_speed = 115200
lib_deps = olikraus/U8g2@^2.35.30
build_flags = -Wall
```

---

## Flashing

### Arduino IDE

1. **File -> Preferences -> Additional Board Manager URLs**:
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`
2. **Tools -> Board -> Boards Manager**, install **esp8266 by ESP8266 Community**.
3. **Sketch -> Include Library -> Manage Libraries**, install **U8g2** by oliver.
4. Open `flock-mini.ino` (`flock_sigs.h` appears as a second tab).
5. Board settings for the HW-364A:
   - Board: **NodeMCU 1.0 (ESP-12E Module)**
   - Flash Size: **4MB (FS:2MB OTA:~1019KB)**
   - Upload Speed: **115200** - these clones drop uploads at 921600
   - Port: whatever appears when you plug in. If nothing appears, install the
     [CH340 driver](https://sparks.gogo.co.nz/ch340.html).
6. Upload.

### PlatformIO

```
pip install platformio
pio run -t upload
pio device monitor
```

## Verifying it works

You almost certainly have no camera in range right now, so prove the plumbing first.

1. **Sniffer alive?** Serial at 115200 should print a `[flock-mini]` status line every 30 seconds
   with a rising `heap` and rotating `ch`.
2. **Low tier working?** `a4:cf:12`, `3c:71:bf` and `08:3a:88` are Espressif prefixes, so almost any
   smart plug, bulb, or dev board nearby will register as a low-confidence hit within a minute. That
   confirms promiscuous capture, OUI matching, channel hopping, and the display.
3. **High tier working?** This is the one that matters, since it is the path a real camera trips.
   Find your phone's WiFi MAC (Android: Settings -> About -> Status; iOS: Settings -> General ->
   About -> Wi-Fi Address - disable private/randomized MAC for your test), then set
   `#define SELFTEST_OUI 0xAABBCC` to its first three bytes and reflash. Turn WiFi off and on to
   make the phone scan. A `FLOCK DETECTED` screen with method `WPROBE` means the full wildcard-probe
   detector works. Comment the define out and reflash when done.

## Notes and caveats

- **No BLE.** The ESP8266 has no Bluetooth radio, so upstream's BLE detection (Flock manufacturer ID
  `0x09C8`, "Penguin" devices) is not possible on this board. WiFi only.
- **Low tier is noisy by design.** It is recorded but never buzzes, because a bare OUI match on an
  Espressif or Liteon prefix is weak evidence. Treat `HI` rows as meaningful and `lo` rows as leads.
- **RSSI is not distance.** CLOSE/NEAR/FAR is a rough hint; walls, poles, and antenna orientation
  move it by tens of dB.
- Detections live in RAM only (64 slots) and reset on reboot. That is deliberate: ESP8266 flash
  writes stall the radio and would drop frames mid-capture.
- If the link step ever complains that `.text` will not fit in `iram1_0_seg` after you add features,
  it is IRAM pressure, not a code bug; nothing here is marked `IRAM_ATTR` precisely to leave headroom.
- **Legal.** This firmware only listens to management frames that are broadcast in the clear, and it
  transmits nothing. Laws vary by jurisdiction; check yours.
