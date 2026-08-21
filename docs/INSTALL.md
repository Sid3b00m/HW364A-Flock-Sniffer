# flock-mini: full install, start to finish

Windows 10/11 + PowerShell + HW-364A (ESP8266 with the onboard 0.96" SSD1306).
Everything below runs from the project folder - the one containing `FIRMWARE.md`. Open a shell there
with Shift + right-click inside the folder, then **Open PowerShell window here**.

Roughly 15 minutes, most of it downloads.

> **Fast path:** `INSTALLER.md` automates Steps 2 through 4 in one command. Do Step 1 first (the
> CH340 driver), then run the installer. Read on if you want to understand each step or something
> goes wrong.

---

## Step 0 - what you need

- The HW-364A board.
- A **USB-C data cable**. Charge-only cables are the single most common reason a board never
  appears as a COM port. If in doubt, use the cable that came with a phone or an external drive.
- Python 3.12, already installed on this machine. Verify: `python --version`.

---

## Step 1 - CH340 driver and a COM port

The HW-364A talks over a CH340 USB-serial bridge, which Windows does not ship a driver for.

1. Download and run the driver from [sparks.gogo.co.nz/ch340.html](https://sparks.gogo.co.nz/ch340.html)
   (or the vendor page at [wch-ic.com](https://www.wch-ic.com/downloads/CH341SER_EXE.html)).
2. Plug the board in. The OLED will likely show random pixels or stay dark - normal for a board with
   no firmware.
3. Confirm Windows sees it:

```powershell
Get-PnpDevice -Class Ports | Format-Table Status,FriendlyName
```

You are looking for a new entry such as `USB-SERIAL CH340 (COM5)`. **Write down that COM number.**

Your machine already has legacy `COM1` and `COM2` motherboard ports that are not your board. This
matters: auto-detection can pick the wrong one, so we will pin the port explicitly in Step 3.

If no new port appears: try a different cable first, then a different USB port, then reboot after
the driver install.

---

## Step 2 - materialise the source files

The three source files live as code blocks inside `FIRMWARE.md`. This extracts them to disk:

```powershell
$md = Get-Content .\FIRMWARE.md -Raw
$f = [string][char]0x60 * 3
$b = [regex]::Matches($md, "(?ms)^$f(cpp|ini)\r?\n(.*?)^$f")
$cpp = @($b | Where-Object { $_.Groups[1].Value -eq 'cpp' })
$ini = @($b | Where-Object { $_.Groups[1].Value -eq 'ini' })
$enc = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText("$pwd\flock_sigs.h",   $cpp[0].Groups[2].Value, $enc)
[IO.File]::WriteAllText("$pwd\flock-mini.ino", $cpp[1].Groups[2].Value, $enc)
[IO.File]::WriteAllText("$pwd\platformio.ini", $ini[0].Groups[2].Value, $enc)
Get-ChildItem *.ino,*.h,*.ini
```

You should end up with `flock-mini.ino`, `flock_sigs.h`, and `platformio.ini`. Sanity check that the
sketch is complete (expect roughly 500 lines):

```powershell
(Get-Content .\flock-mini.ino).Count
```

---

## Step 3 - install PlatformIO and build

PlatformIO downloads the ESP8266 toolchain and the U8g2 library on its own, so this is the whole
toolchain in one command. Roughly 300 MB on first run.

```powershell
pip install platformio
```

If `pio` is not recognised afterwards, its Scripts directory is not on PATH. Use the module form
instead - it always works:

```powershell
python -m platformio run
```

### Pin the upload port

Because of those legacy COM1/COM2 ports, tell PlatformIO exactly which port to use. Open
`platformio.ini` and add two lines under `[env:hw364a]`, substituting your COM number from Step 1:

```ini
upload_port = COM5
monitor_port = COM5
```

### Compile first, flash second

```powershell
python -m platformio run
```

This compiles without touching the board. A successful run ends with `[SUCCESS]` and a memory
summary; expect RAM around 40% and flash around 30%. If it fails, stop here and fix the error before
plugging anything in.

Then flash:

```powershell
python -m platformio run -t upload
```

The CH340's DTR/RTS lines normally reset the board into its bootloader automatically. If the upload
stalls at `Connecting....`, do the manual bootloader dance: **hold FLASH, tap RST, release FLASH**,
then immediately re-run the upload.

---

## Step 3b - Arduino IDE instead (alternative)

Only if you would rather have a GUI. Skip if Step 3 worked.

1. Install the IDE from [arduino.cc/en/software](https://www.arduino.cc/en/software).
2. **File > Preferences > Additional Board Manager URLs**:
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`
3. **Tools > Board > Boards Manager** - install **esp8266 by ESP8266 Community**.
4. **Sketch > Include Library > Manage Libraries** - install **U8g2** by oliver.
5. Open `flock-mini.ino` from the project folder. The folder name must match the sketch name, so if
   you renamed the folder, rename it back to `flock-mini`. `flock_sigs.h` shows up as a second tab.
6. **Tools** settings:
   - Board: **NodeMCU 1.0 (ESP-12E Module)**
   - Flash Size: **4MB (FS:2MB OTA:~1019KB)**
   - Upload Speed: **115200** - these clones fail at 921600
   - Port: your CH340 port
7. Click **Upload**.

---

## Step 4 - first boot

Open the serial monitor:

```powershell
python -m platformio device monitor -b 115200
```

(`Ctrl+C` exits. In Arduino IDE it is **Tools > Serial Monitor**, set to 115200 baud.)

Expected output, in order:

```
[flock-mini] boot
[flock-mini] oled sda=14 scl=12
[flock-mini] sniffing, 32 signatures, heap=42000
[flock-mini] ch=6 seen=0 high=0 drops=0 heap=41800
```

That third line confirms the radio is in promiscuous mode. The status line then repeats every 30
seconds with `ch` rotating through 1, 6, and 11.

On the display: a `FLOCK-MINI / passive rx only / 32 signatures` splash for about a second, then the
scan screen with an animated `Scanning...`, a live channel number in the top right, and an uptime
and free-heap readout along the bottom.

If the serial output looks right but the screen is dark, look for the line
`WARNING: no I2C device at 0x3C on either pin order` - see troubleshooting below.

---

## Step 5 - prove it actually detects

You almost certainly have no Flock camera in range, so test the machinery instead. Do these in order,
because each one tests strictly more than the last.

**5a. Sniffer alive.** Status lines every 30 seconds with a rotating channel and a stable heap. If
heap falls steadily over several minutes, something is leaking - tell me.

**5b. Low-confidence tier.** Three prefixes in the list (`a4:cf:12`, `3c:71:bf`, `08:3a:88`) belong to
Espressif, so any ESP-based smart plug, bulb, or dev board nearby will register within a minute or
two. Look for `"confidence":"low"` in the serial JSON and a `lo` row on the list screen. This proves
promiscuous capture, OUI matching, channel hopping, and the display all work end to end.

**5c. High-confidence tier.** This is the path a real camera trips, so it is the one worth proving.

1. Find your phone's WiFi MAC. Android: **Settings > About phone > Status > WiFi MAC address**.
   iPhone: **Settings > General > About > Wi-Fi Address**. Turn off private/randomised MAC for your
   own network first, or you will be testing a MAC that changes.
2. Take the first three bytes. For `a1:b2:c3:d4:e5:f6` that is `A1B2C3`.
3. In `flock-mini.ino`, find and uncomment the self-test line, filling in your value:

```cpp
#define SELFTEST_OUI 0xA1B2C3
```

4. Reflash, then toggle the phone's WiFi off and on so it scans.
5. Within seconds you should get a `FLOCK DETECTED` screen showing method `WPROBE`, plus serial JSON
   with `"detection_method":"wifi_wildcard_probe"`.

That confirms the full wildcard-probe detector: management-frame capture, information-element
parsing, the zero-length SSID test, and the alert path. **Comment the define out and reflash when
you are done**, or your phone will set the alarm off forever.

---

## Reading the device in the field

**Scan screen** - channel, how many devices are currently in range (seen in the last 10 seconds), and
running totals as `high:N low:N`.

**Alert screen** - fires for 4 seconds on a new high-confidence detection: MAC, method, RSSI with a
CLOSE/NEAR/FAR hint, channel, hit count, and SSID if there was one.

**List screen** - everything seen this session, newest first, each row tagged `HI` or `lo`, with `<`
meaning still in range.

**FLASH button** - tap to page through the list, hold about a second to mute alerts (the footer
switches to `MUTE`).

Treat `HI` rows as meaningful and `lo` rows as leads worth a second look, not as cameras. Detections
are held in RAM only and clear on reboot; the serial JSON is your permanent record if you want one:

```powershell
python -m platformio device monitor -b 115200 | Tee-Object -FilePath flock-log.txt
```

---

## Troubleshooting

**No COM port.** Cable first (must carry data), then the CH340 driver, then a reboot. `Get-PnpDevice
-Class Ports` should list a CH340 entry.

**Upload stalls at `Connecting....`.** Hold FLASH, tap RST, release FLASH, re-run upload. Also confirm
`upload_port` points at the CH340 port and not COM1/COM2.

**Board resets or browns out when scanning starts.** Known weakness of these clones: the radio's
current draw exceeds what a weak USB port delivers. Use a powered hub, a rear-panel port, or feed 5V
into `VIN`. This is a power problem, not a firmware problem.

**Screen dark but serial looks healthy.** Check the `oled sda=` line. If you see the `no I2C device at
0x3C` warning, the panel is either at address `0x3D` (change `OLED_ADDR`) or on different pins. Both
pin orders, 14/12 and 12/14, are probed automatically already.

**Serial prints garbage.** Baud mismatch - it must be 115200. A burst of gibberish at the very start
is normal: that is the ESP8266 ROM bootloader talking at 74880 baud.

**`pio` not recognised.** Use `python -m platformio ...` for every command.

**Compile error mentioning `iram1_0_seg`.** Only relevant if you start adding features. Nothing here
is marked `IRAM_ATTR` specifically to leave that headroom.

**Zero detections ever, not even low-confidence ones.** Confirm `drops=0` and a rotating `ch` in the
status line, then re-run the 5c self-test. If your phone does not trip it, the sniffer is not
receiving and something is wrong at the radio layer - bring me the serial log.

---

## Reminder on what this is

A passive receiver. It never transmits, never deauthenticates, never jams, never associates. It only
reads 802.11 management frames that are already being broadcast in the clear. Laws vary by
jurisdiction; check yours.

It also cannot see Bluetooth. The ESP8266 has no BLE radio, so upstream's BLE detection path (Flock
manufacturer ID `0x09C8`, "Penguin" devices) is out of reach on this board. If you later want that,
it needs an ESP32.
