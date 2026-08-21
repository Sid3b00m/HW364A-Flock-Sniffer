// flock_selftest.h - synthetic frame self-test, built only by hw364a-selftest.
//
// This lives in a header rather than in the .ino for the reason described in
// flock_sigs.h: the Arduino build generates a prototype for every function it
// finds in the sketch, and it does so without running the preprocessor first.
// Code behind #ifdef in the .ino therefore still gets prototyped in builds
// where it does not exist, which warns on every function. Headers are not
// scanned, so putting it here keeps the shipping build clean.
//
// It is included partway down flock-mini.ino, on purpose: it uses that file's
// file-scope statics (sniffCb, dets, hitQueue, drainHits, ...) and so has to
// come after them.

#pragma once

static uint16_t stPass = 0, stFail = 0;

static void stReset() {
  memset(dets, 0, sizeof(dets));
  detCount = 0;
  qHead = qTail = 0;
  qDropped = 0;
  alertIdx = -1;
}

static void stCheck(const char* name, bool ok) {
  if (ok) stPass++; else stFail++;
  Serial.printf("[test] %-30s %s\n", name, ok ? "PASS" : "FAIL");
}

// sniffer_buf2 as the SDK hands it over: rx_ctrl[12] + buf[112] + cnt[2] + len[2].
// declaredLen goes in the trailing len field, which is what the parser must read.
static void stMgmt(uint8_t* out, const uint8_t* frame, uint16_t copyLen,
                   int8_t rssi, uint8_t ch, uint16_t declaredLen) {
  memset(out, 0, 128);
  out[0] = (uint8_t)rssi;
  out[10] = ch & 0x0f;
  if (copyLen > 112) copyLen = 112;
  memcpy(out + 12, frame, copyLen);
  out[124] = 1;                                 // cnt
  out[126] = (uint8_t)(declaredLen & 0xff);     // len
  out[127] = (uint8_t)(declaredLen >> 8);
}

static void stData(uint8_t* out, const uint8_t* a1, const uint8_t* a2,
                   int8_t rssi, uint8_t ch) {
  memset(out, 0, 60);
  out[0] = (uint8_t)rssi;
  out[10] = ch & 0x0f;
  uint8_t* f = out + 12;
  f[0] = 0x08;                                  // data frame
  memcpy(f + 4, a1, 6);
  memcpy(f + 10, a2, 6);
  memset(f + 16, 0x22, 6);
}

static uint16_t stProbeReq(uint8_t* f, const uint8_t* a2, const char* ssid) {
  memset(f, 0, 112);
  f[0] = 0x40;                                  // management, subtype 4
  memset(f + 4, 0xff, 6);
  memcpy(f + 10, a2, 6);
  memset(f + 16, 0xff, 6);
  uint8_t n = ssid ? (uint8_t)strlen(ssid) : 0;
  f[24] = 0;                                    // SSID element
  f[25] = n;
  if (n) memcpy(f + 26, ssid, n);
  return (uint16_t)(26 + n);
}

static uint16_t stBeacon(uint8_t* f, const uint8_t* a2, const char* ssid) {
  memset(f, 0, 112);
  f[0] = 0x80;                                  // management, subtype 8
  memset(f + 4, 0xff, 6);
  memcpy(f + 10, a2, 6);
  memcpy(f + 16, a2, 6);
  uint8_t n = (uint8_t)strlen(ssid);
  f[36] = 0;                                    // SSID element after the 12 fixed bytes
  f[37] = n;
  memcpy(f + 38, ssid, n);
  return (uint16_t)(38 + n);
}

// Returns the method of the resulting hit, or -1 if the frame was ignored.
static int stInject(uint8_t* buf, uint16_t len) {
  qHead = qTail = 0;
  sniffCb(buf, len);
  if (qHead == qTail) return -1;
  return (int)hitQueue[qTail].method;
}

static void runFrameSelftest() {
  uint8_t frame[112], buf[128];
  const uint8_t tgt[6]   = {0x70, 0xc9, 0x4e, 0x11, 0x22, 0x33};
  const uint8_t other[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  uint16_t n;

  Serial.println();
  Serial.println("[test] synthetic frame self-test");

  stReset();
  n = stProbeReq(frame, tgt, NULL);
  stMgmt(buf, frame, n, -55, 6, n);
  stCheck("wildcard probe -> HIGH", stInject(buf, 128) == M_WILDCARD_PROBE);

  // A header-only frame has no elements. Everything past the header is zero,
  // which looks exactly like an empty SSID element, so a parser that reads the
  // wrong length field invents a wildcard probe that was never transmitted.
  stReset();
  stMgmt(buf, frame, 24, -55, 6, 24);
  stCheck("truncated -> no false probe", stInject(buf, 128) == M_OUI_TX);

  stReset();
  n = stBeacon(frame, other, "FlockSafety-Cam");
  stMgmt(buf, frame, n, -60, 6, n);
  stCheck("ssid keyword -> HIGH", stInject(buf, 128) == M_SSID_KEYWORD);

  stReset();
  n = stProbeReq(frame, tgt, NULL);
  frame[25] = 200;                              // element longer than the frame
  stMgmt(buf, frame, n, -55, 6, n);
  stCheck("oversized element is safe", stInject(buf, 128) == M_OUI_TX);

  stReset();
  stData(buf, tgt, other, -70, 11);
  stCheck("data frame receiver -> OUI-RX", stInject(buf, 60) == M_OUI_RX);

  stReset();
  n = stProbeReq(frame, other, NULL);
  stMgmt(buf, frame, n, -55, 6, n);
  stCheck("unknown prefix ignored", stInject(buf, 128) == -1);

  stReset();
  n = stProbeReq(frame, tgt, NULL);
  stMgmt(buf, frame, n, -110, 6, n);
  stCheck("below rssi floor ignored", stInject(buf, 128) == -1);

  stCheck("channel 1 is 2412 MHz",  chanFreq(1)  == 2412);
  stCheck("channel 11 is 2462 MHz", chanFreq(11) == 2462);
  stCheck("channel 14 is 2484 MHz", chanFreq(14) == 2484);

  // Fill every slot with low-confidence noise, then prove a real camera still
  // gets in. Three of the shipped prefixes are Espressif, so this is the normal
  // state of the table in a built-up area rather than a corner case.
  stReset();
  for (uint16_t i = 0; i < MAX_DETECTIONS; i++) {
    const uint8_t mac[6] = {0x70, 0xc9, 0x4e, 0x00, (uint8_t)(i >> 8), (uint8_t)i};
    stProbeReq(frame, mac, NULL);
    stMgmt(buf, frame, 24, -80, 6, 24);
    sniffCb(buf, 128);
    drainHits();
  }
  stCheck("table fills with low hits", detCount == MAX_DETECTIONS && countHigh() == 0);

  const uint8_t cam[6] = {0x70, 0xc9, 0x4e, 0xaa, 0xbb, 0xcc};
  n = stProbeReq(frame, cam, NULL);
  stMgmt(buf, frame, n, -50, 6, n);
  sniffCb(buf, 128);
  drainHits();
  stCheck("full table still takes HIGH", findDet(cam) >= 0 && countHigh() == 1);

  const uint8_t junk[6] = {0x70, 0xc9, 0x4e, 0xde, 0xad, 0x01};
  stProbeReq(frame, junk, NULL);
  stMgmt(buf, frame, 24, -80, 6, 24);
  sniffCb(buf, 128);
  drainHits();
  stCheck("full table drops extra low", findDet(junk) < 0 && detCount == MAX_DETECTIONS);

  stReset();
  Serial.printf("[test] %u passed, %u failed\n", (unsigned)stPass, (unsigned)stFail);
  Serial.println();
}
