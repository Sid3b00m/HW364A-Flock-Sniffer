# flock-mini web flasher

Flash the board from a Chrome or Edge tab. Nothing to install on the machine doing the flashing -
no Python, no PlatformIO, no Arduino IDE. This is the right way to hand a working detector to
someone else.

It uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/), which talks to the board over the
browser's Web Serial API.

**You still build the firmware once**, on this machine. After that, the `web` folder is portable.

Requirements on the flashing machine:

- Chrome or Edge on desktop. Web Serial does not exist in Firefox or Safari.
- The CH340 driver, since that is an operating-system driver the browser cannot replace:
  [sparks.gogo.co.nz/ch340.html](https://sparks.gogo.co.nz/ch340.html)
- The page served from `localhost` or over HTTPS. Web Serial refuses plain `http://` on a remote
  host. The script below handles localhost for you.

---

## Set it up

Paste this into PowerShell. It writes the web files, builds the firmware, and opens the flasher.

```
$f = [string][char]0x60 * 3
$d = if (Test-Path .\docs\WEBFLASH.md) { '.\docs' } else { '.' }
$m = Get-Content "$d\WEBFLASH.md" -Raw
function Grab($lang) { [regex]::Match($m, "(?ms)^${f}$lang\r?\n(.*?)^${f}").Groups[1].Value }
$enc = New-Object System.Text.UTF8Encoding($false)
New-Item -ItemType Directory -Force -Path .\web | Out-Null
[IO.File]::WriteAllText("$pwd\web\index.html",    (Grab 'html'),       $enc)
[IO.File]::WriteAllText("$pwd\web\manifest.json", (Grab 'json'),       $enc)
[IO.File]::WriteAllText("$pwd\webflash.ps1",      (Grab 'powershell'), $enc)
powershell -ExecutionPolicy Bypass -File .\webflash.ps1
```

Your browser opens on `http://localhost:8000`. Click **Flash flock-mini**, pick the CH340 port in
the browser's port chooser, and wait. `Ctrl+C` in the terminal stops the server when you are done.

Re-run later with `.\webflash.ps1`, or `.\webflash.ps1 -NoBuild` to skip recompiling.

---

## Sharing it

The `web` folder is now self-contained: `index.html`, `manifest.json`, `firmware.bin`. Push those
three files to a GitHub Pages repo and anyone with Chrome, the CH340 driver, and a board can flash
it from your URL with nothing installed. GitHub Pages serves HTTPS, which satisfies Web Serial.

Do not rename `firmware.bin` without editing `manifest.json` to match.

---

## `web/index.html`

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>flock-mini web flasher</title>
<script type="module"
        src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
<style>
  :root { color-scheme: dark; }
  body {
    margin: 0; min-height: 100vh; display: grid; place-items: center;
    background: #0d1117; color: #c9d1d9;
    font: 15px/1.6 -apple-system, "Segoe UI", system-ui, sans-serif;
  }
  main { width: min(560px, 90vw); padding: 40px 0; }
  h1 { font-size: 28px; margin: 0 0 4px; color: #f0f6fc; letter-spacing: -0.02em; }
  .sub { color: #8b949e; margin: 0 0 28px; }
  .card {
    background: #161b22; border: 1px solid #30363d; border-radius: 10px;
    padding: 22px; margin-bottom: 18px;
  }
  ol { margin: 0; padding-left: 20px; }
  li { margin-bottom: 8px; }
  code { background: #21262d; padding: 1px 6px; border-radius: 4px; font-size: 13px; }
  .cta { text-align: center; padding: 26px 0 6px; }
  .warn { color: #d29922; font-size: 13px; }
  a { color: #58a6ff; }
  footer { color: #6e7681; font-size: 12px; text-align: center; margin-top: 22px; }
  esp-web-install-button { --esp-tools-button-color: #f0f6fc; }
</style>
</head>
<body>
<main>
  <h1>flock-mini</h1>
  <p class="sub">Passive Flock Safety camera detector &middot; ESP8266 + 0.96&quot; OLED</p>

  <div class="card">
    <ol>
      <li>Plug the board in with a <strong>data</strong> USB cable.</li>
      <li>Click the button below, then choose the <code>USB-SERIAL CH340</code> port.</li>
      <li>Wait for the progress bar. The board reboots into the detector by itself.</li>
    </ol>
    <div class="cta">
      <esp-web-install-button manifest="manifest.json">
        <button slot="activate">Flash flock-mini</button>
        <span slot="unsupported" class="warn">
          This browser cannot flash. Use Chrome or Edge on a desktop.
        </span>
        <span slot="not-allowed" class="warn">
          Serial access needs HTTPS or localhost.
        </span>
      </esp-web-install-button>
    </div>
  </div>

  <div class="card">
    <strong>No port in the list?</strong>
    <p style="margin:8px 0 0">
      Install the <a href="https://sparks.gogo.co.nz/ch340.html">CH340 driver</a> and re-plug the
      board. If it still fails, the cable is charge-only &mdash; swap it.
    </p>
  </div>

  <footer>
    Passive receiver. Transmits nothing. Based on
    <a href="https://github.com/colonelpanichacks/flock-you">flock-you</a>.
  </footer>
</main>
</body>
</html>
```

## `web/manifest.json`

For the ESP8266 the PlatformIO output is already a complete flash image, so it goes at offset 0.

```json
{
  "name": "flock-mini",
  "version": "1.0.0",
  "home_assistant_domain": null,
  "new_install_prompt_erase": true,
  "improv": false,
  "builds": [
    {
      "chipFamily": "ESP8266",
      "parts": [
        { "path": "firmware.bin", "offset": 0 }
      ]
    }
  ]
}
```

## `webflash.ps1`

```powershell
param(
    [int]$ServerPort = 8000,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
Set-Location $root

function Step([string]$m) { Write-Host ''; Write-Host "==> $m" -ForegroundColor Cyan }
function Ok([string]$m)   { Write-Host "    $m" -ForegroundColor Green }
function Note([string]$m) { Write-Host "    $m" -ForegroundColor Gray }

if (-not (Test-Path '.\web\index.html')) {
    throw 'web\index.html missing. Run the setup block in WEBFLASH.md first.'
}

if ($NoBuild) {
    Step 'Skipping build (-NoBuild)'
}
else {
    Step 'Building firmware'
    if (-not (Test-Path '.\platformio.ini')) {
        throw 'platformio.ini missing. Run the installer from INSTALLER.md first.'
    }
    & python -m platformio run
    if ($LASTEXITCODE -ne 0) { throw 'Compile failed. Fix the first error above.' }
    Ok 'Built'
}

Step 'Staging firmware.bin'
$bin = Get-ChildItem '.\.pio\build\*\firmware.bin' -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $bin) { throw 'No firmware.bin found under .pio\build. Build first.' }
Copy-Item $bin.FullName '.\web\firmware.bin' -Force
Ok ("firmware.bin  {0:N0} KB" -f ($bin.Length / 1KB))

Step 'Serving the flasher'
$url = "http://localhost:$ServerPort/"
Note $url
Note 'Chrome or Edge only. Ctrl+C here to stop the server.'

Start-Process powershell -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-Command', "Start-Sleep -Seconds 2; Start-Process '$url'"
)

& python -m http.server $ServerPort --directory .\web
```
