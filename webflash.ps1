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
