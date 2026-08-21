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

// Channel 14 breaks the 5 MHz spacing.
static uint16_t chanFreq(uint8_t ch) {
  return (ch == 14) ? 2484 : (uint16_t)(2412 + (ch - 1) * 5);
}

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
    // sniffer_buf2 is rx_ctrl[12] + buf[112] + cnt[2] + len[2], so the real
    // frame length is the last field at offset 126. Offset 124 is cnt.
    uint16_t real = (uint16_t)buf[126] | ((uint16_t)buf[127] << 8);
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

// The low-confidence entry unheard from the longest, or -1 if every slot holds
// a high-confidence detection.
static int stalestLowConfidence() {
  int worst = -1;
  uint32_t worstAge = 0, now = millis();
  for (uint8_t i = 0; i < detCount; i++) {
    if (isHighConfidence(dets[i].method)) continue;
    uint32_t age = now - dets[i].lastSeen;
    if (worst < 0 || age > worstAge) { worst = i; worstAge = age; }
  }
  return worst;
}

static void handleHit(const Hit& h) {
  uint32_t now = millis();
  int idx = findDet(h.mac);
  bool worthAlert = false;

  if (idx < 0) {
    if (detCount < MAX_DETECTIONS) {
      idx = detCount++;
    } else {
      // Table full. Three of the shipped prefixes belong to Espressif, so in a
      // dense area it fills with low-confidence noise. Recycle the stalest of
      // that rather than let it lock a real camera out of the table entirely.
      if (!isHighConfidence(h.method)) return;
      idx = stalestLowConfidence();
      if (idx < 0) return;
    }
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
      h.rssi, (unsigned)h.ch, (unsigned)chanFreq(h.ch), d.ssid, (unsigned)d.hits);
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

// ---------------------------------------------------------------- self-test
//
// Kept in a header so the Arduino prototype generator does not emit prototypes
// for it in builds where SELFTEST_FRAMES is undefined. Included here, rather
// than at the top, because it uses the statics defined above.

#ifdef SELFTEST_FRAMES
#include "flock_selftest.h"
#endif

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
  int y = 17;   // 5 rows at 8px keeps the last one clear of the divider at 53
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

    y += 8;
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
#ifdef SELFTEST_FRAMES
  runFrameSelftest();
#endif
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
