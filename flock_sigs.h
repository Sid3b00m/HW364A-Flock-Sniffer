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
