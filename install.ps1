#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Port,
    [switch]$NoFlash,
    [switch]$KeepSource,
    [switch]$Monitor,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'

function Step([string]$m) { Write-Host ''; Write-Host "==> $m" -ForegroundColor Cyan }
function Ok([string]$m)   { Write-Host "    $m" -ForegroundColor Green }
function Note([string]$m) { Write-Host "    $m" -ForegroundColor Gray }
function Warn([string]$m) { Write-Host "    $m" -ForegroundColor Yellow }

function Confirm([string]$question) {
    if ($Yes) { return $true }
    $a = Read-Host "$question [Y/n]"
    return ($a -eq '' -or $a -match '^[Yy]')
}

$root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
Set-Location $root

Write-Host ''
Write-Host '  flock-mini installer' -ForegroundColor White
Write-Host "  $root" -ForegroundColor DarkGray

# ---------------------------------------------------------------- 1. preflight

Step 'Checking prerequisites'

if (Test-Path '.\FIRMWARE.md') {
    Ok 'FIRMWARE.md found'
}
elseif ((Test-Path '.\flock-mini.ino') -and (Test-Path '.\platformio.ini')) {
    Ok 'Source files found'
}
else {
    throw "Neither FIRMWARE.md nor the source files are in $root. Run this from the project folder."
}

$pyVersion = $null
try { $pyVersion = (& python --version 2>&1 | Out-String).Trim() } catch { }
if (-not $pyVersion -or $LASTEXITCODE -ne 0) {
    throw 'Python not found on PATH. Install it from https://www.python.org/downloads/ (tick "Add python.exe to PATH").'
}
Ok $pyVersion

# ---------------------------------------------------------------- 1b. launcher

Step 'Creating a double-click launcher'

$cmdPath = Join-Path $root 'Install flock-mini.cmd'
$cmdBody = @(
    '@echo off',
    'cd /d "%~dp0"',
    'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"',
    'pause'
) -join "`r`n"
Set-Content -Path $cmdPath -Value $cmdBody -Encoding ASCII
Ok 'Install flock-mini.cmd'

try {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $lnk = Join-Path $desktop 'Install flock-mini.lnk'
    $sc = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
    $sc.TargetPath = $cmdPath
    $sc.WorkingDirectory = $root
    $sc.Save()
    Ok 'Desktop icon created - double-click that next time'
}
catch {
    Warn 'Could not create the Desktop icon. Harmless - use the .cmd in this folder.'
}

# ---------------------------------------------------------------- 2. sources

$haveSource = @('flock-mini.ino', 'flock_sigs.h', 'platformio.ini') |
              ForEach-Object { Test-Path ".\$_" } | Where-Object { -not $_ } | Measure-Object
$haveSource = ($haveSource.Count -eq 0)

if ($KeepSource -or (-not (Test-Path '.\FIRMWARE.md'))) {
    Step 'Using the source already on disk'
    if (-not $haveSource) {
        throw 'Source files are missing and there is no FIRMWARE.md to extract them from.'
    }
    Ok 'flock-mini.ino, flock_sigs.h, platformio.ini'
}
else {
    Step 'Extracting source from FIRMWARE.md'

    $fence = [string][char]0x60 * 3
    $pattern = "(?ms)^$fence(cpp|ini)\r?\n(.*?)^$fence"
    $md = Get-Content '.\FIRMWARE.md' -Raw
    $blocks = [regex]::Matches($md, $pattern)

    $cpp = @($blocks | Where-Object { $_.Groups[1].Value -eq 'cpp' })
    $ini = @($blocks | Where-Object { $_.Groups[1].Value -eq 'ini' })

    if ($cpp.Count -lt 2 -or $ini.Count -lt 1) {
        throw "FIRMWARE.md does not contain the expected code blocks (found $($cpp.Count) cpp, $($ini.Count) ini)."
    }

    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $targets = @(
        @{ Name = 'flock_sigs.h';   Body = $cpp[0].Groups[2].Value },
        @{ Name = 'flock-mini.ino'; Body = $cpp[1].Groups[2].Value },
        @{ Name = 'platformio.ini'; Body = $ini[0].Groups[2].Value }
    )

    foreach ($t in $targets) {
        $path = Join-Path $root $t.Name
        if (Test-Path $path) {
            $existing = [IO.File]::ReadAllText($path)
            # platformio.ini gets rewritten with the port below, so never nag about it
            if ($existing -ne $t.Body -and $t.Name -ne 'platformio.ini') {
                Copy-Item $path "$path.bak" -Force
                Warn "$($t.Name) differed - previous version saved as $($t.Name).bak"
            }
        }
        [IO.File]::WriteAllText($path, $t.Body, $utf8)
        $lines = ($t.Body -split "`n").Count
        Ok "$($t.Name) ($lines lines)"
    }
}

if (-not (Test-Path '.\FIRMWARE.md')) {
    Note 'No FIRMWARE.md here, which is normal for a git clone: the source files are canonical.'
}

# ---------------------------------------------------------------- 3. platformio

Step 'Checking PlatformIO'

$pioOk = $false
try {
    $v = (& python -m platformio --version 2>&1 | Out-String).Trim()
    $pioOk = ($LASTEXITCODE -eq 0)
    if ($pioOk) { Ok $v }
} catch { }

if (-not $pioOk) {
    Note 'Not installed. Installing now, this takes a minute...'
    & python -m pip install --upgrade platformio
    if ($LASTEXITCODE -ne 0) { throw 'pip install platformio failed. Check your network and try again.' }
    $v = (& python -m platformio --version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'PlatformIO installed but will not run.' }
    Ok $v
}

# ---------------------------------------------------------------- 4. serial port

Step 'Looking for the board'

if (-not $Port) {
    $candidates = @()
    try {
        $candidates = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '\(COM\d+\)' } |
            ForEach-Object {
                [pscustomobject]@{
                    Port   = [regex]::Match($_.Name, '\((COM\d+)\)').Groups[1].Value
                    Name   = $_.Name
                    Likely = ($_.Name -match 'CH340|CH341|USB-SERIAL|USB Serial|CP210|Silicon Labs|FTDI|Prolific')
                }
            }
    } catch { }

    foreach ($c in $candidates) { Note "$($c.Port)  $($c.Name)" }

    $likely = @($candidates | Where-Object { $_.Likely })

    if ($likely.Count -eq 1) {
        $Port = $likely[0].Port
        Ok "Using $Port"
    }
    elseif ($likely.Count -gt 1) {
        if ($Yes) {
            $Port = $likely[0].Port
            Warn "Several USB serial ports found, using $Port"
        }
        else {
            Write-Host ''
            $Port = Read-Host "Several USB serial ports found. Which one? (e.g. $($likely[0].Port))"
        }
    }
    else {
        Warn 'No USB serial adapter detected.'
        Warn 'The board is unplugged, the cable is charge-only, or the CH340 driver is missing:'
        Warn '  https://sparks.gogo.co.nz/ch340.html'
        Warn 'Continuing with a compile-only build.'
        $NoFlash = $true
    }
}
else {
    Ok "Using $Port (given on the command line)"
}

if ($Port) {
    $iniLines = Get-Content '.\platformio.ini' | Where-Object { $_ -notmatch '^\s*(upload_port|monitor_port)\s*=' }
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($line in $iniLines) {
        $out.Add($line) | Out-Null
        if ($line -match '^\s*\[env:') {
            $out.Add("upload_port = $Port") | Out-Null
            $out.Add("monitor_port = $Port") | Out-Null
        }
    }
    Set-Content '.\platformio.ini' -Value $out -Encoding ASCII
    Ok "Pinned $Port in platformio.ini"
}

# ---------------------------------------------------------------- 5. compile

Step 'Compiling'
Note 'First run downloads the ESP8266 toolchain and U8g2 (~300 MB).'

& python -m platformio run
if ($LASTEXITCODE -ne 0) {
    throw 'Compile failed. Scroll up for the first error - that is the real one.'
}
Ok 'Build succeeded'

# ---------------------------------------------------------------- 6. flash

if ($NoFlash) {
    Step 'Skipping flash'
}
elseif (Confirm "Flash the board on $Port now?") {
    Step "Flashing $Port"
    & python -m platformio run -t upload
    if ($LASTEXITCODE -ne 0) {
        Write-Host ''
        Warn 'Upload failed. Two things fix almost every case:'
        Warn '  1. Manual bootloader: hold FLASH, tap RST, release FLASH, then re-run.'
        Warn '  2. Weak USB power. Use a rear port or a powered hub.'
        throw 'Upload failed.'
    }
    Ok 'Flashed'
}
else {
    Step 'Flash skipped'
}

# ---------------------------------------------------------------- 7. done

Write-Host ''
Write-Host '  Done.' -ForegroundColor Green
Note 'Expect on serial: "[flock-mini] sniffing, 32 signatures" then a status line every 30s.'
Note 'Expect on screen: splash, then a scan screen with the channel cycling 1/6/11.'
Write-Host ''

if ($Monitor -or (-not $NoFlash -and (Confirm 'Open the serial monitor?'))) {
    Note 'Ctrl+C to exit.'
    & python -m platformio device monitor -b 115200
}
