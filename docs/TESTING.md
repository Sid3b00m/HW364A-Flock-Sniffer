# Testing

Two ways to gain confidence in the detector without standing under a Flock camera.

## 1. Synthetic frame self-test

A second build environment, `hw364a-selftest`, compiles the normal firmware plus a block of tests
that runs once at boot. It hand-builds 802.11 frames in memory, pushes them through the real
`sniffCb`, and prints what came out. Nothing is transmitted and no radio traffic is involved, so the
result is identical every time and does not depend on what is in the air around you.

```powershell
python -m platformio run -e hw364a-selftest -t upload
python -m platformio device monitor -b 115200
```

Press the RST button if you miss the output; the tests run in `setup()`.

Expected:

```
[test] synthetic frame self-test
[test] wildcard probe -> HIGH          PASS
[test] truncated -> no false probe     PASS
[test] ssid keyword -> HIGH            PASS
[test] oversized element is safe       PASS
[test] data frame receiver -> OUI-RX   PASS
[test] unknown prefix ignored          PASS
[test] below rssi floor ignored        PASS
[test] channel 1 is 2412 MHz           PASS
[test] channel 11 is 2462 MHz          PASS
[test] channel 14 is 2484 MHz          PASS
[test] table fills with low hits       PASS
[test] full table still takes HIGH     PASS
[test] full table drops extra low      PASS
[test] 13 passed, 0 failed
```

The table tests print a burst of detection JSON as they run, which is expected — that is the normal
serial output path being exercised.

What the less obvious cases are guarding:

- **truncated -> no false probe.** A management frame carrying nothing but a 24-byte header. The
  bytes after it are zero, which is byte-for-byte what an empty SSID element looks like, so a parser
  that reads the frame length from the wrong offset reports a high-confidence wildcard probe that
  was never sent. This is the regression test for the `sniffer_buf2` length field living at offset
  126 rather than 124.
- **oversized element is safe.** An element header claiming 200 bytes inside a 26-byte frame, which
  must be rejected rather than walked off the end of the buffer.
- **full table still takes HIGH.** Fills all 64 slots with low-confidence noise and then presents a
  camera. Three of the shipped prefixes belong to Espressif, so a full table of junk is the normal
  state in a built-up area, and a real detection has to be able to displace it.

When you are done, put the normal firmware back. The self-test build wipes the detection table at
boot and is not meant for field use:

```powershell
python -m platformio run -t upload
```

## 2. Live end-to-end check with your phone

The self-test proves the parsing logic. It cannot prove the radio is actually receiving. For that,
set `SELFTEST_OUI` in `flock-mini.ino` to the first three bytes of your phone's WiFi MAC, reflash,
and toggle the phone's WiFi off and on. Phones emit the same wildcard probe requests cameras do, so
a `FLOCK DETECTED` screen reading `WPROBE` means capture, matching, hopping, alerting and display
are all working together. Comment it out afterwards.

Cheaper version: any ESP-based smart plug or bulb in the house trips the low tier within a minute,
because `a4:cf:12` and friends are Espressif prefixes.

## If the upload fails with "Access is denied"

Something else already has the serial port open — usually a serial monitor left running from an
earlier flash. Close it and retry. To find it on Windows:

```powershell
Get-Process | Where-Object { $_.ProcessName -match 'python|putty|arduino' } |
  Select-Object Id, ProcessName, StartTime
```
