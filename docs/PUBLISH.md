# Publish to GitHub

Recompiles with the current fixes, tidies the folder into a repo layout, and pushes it to GitHub as
**HW364A-Flock-Sniffer**. The account name comes from whoever `gh` is signed in as, so nothing
personal is hardcoded.

GitHub repository names cannot contain spaces, so the name becomes `HW364A-Flock-Sniffer` and the
human-readable title "HW364A Flock Sniffer" goes in the description.

## Run it

Short lines, paste the block:

Open PowerShell in the project folder first (Shift + right-click, **Open PowerShell window here**).

```
$f = [string][char]0x60 * 3
$d = if (Test-Path .\docs\PUBLISH.md) { '.\docs' } else { '.' }
$m = Get-Content "$d\PUBLISH.md" -Raw
$p = "(?ms)^${f}powershell\r?\n(.*?)^${f}"
$s = [regex]::Match($m, $p).Groups[1].Value
$e = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText("$pwd\publish.ps1", $s, $e)
$s.Length
powershell -ExecutionPolicy Bypass -File .\publish.ps1
```

`$s.Length` should print several thousand. If it prints 0 the code block did not match.

Options:

- `.\publish.ps1 -Message "what changed"` - commit message for this run.
- `.\publish.ps1 -Private` - make the repo private.
- `.\publish.ps1 -SkipBuild` - do not recompile first.
- `.\publish.ps1 -RepoName something-else` - different repository name.

Safe to re-run. It regenerates the sources from the docs, rebuilds, refreshes
`web/firmware.bin`, and pushes to the existing repository.

---

## The script

```powershell
#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$RepoName = 'HW364A-Flock-Sniffer',
    [string]$Description = 'HW364A Flock Sniffer - passive Flock Safety camera detector for the ESP8266 + 0.96" OLED board',
    [string]$Message = 'Update firmware and docs',
    [switch]$Private,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

function Step([string]$m) { Write-Host ''; Write-Host "==> $m" -ForegroundColor Cyan }
function Ok([string]$m)   { Write-Host "    $m" -ForegroundColor Green }
function Note([string]$m) { Write-Host "    $m" -ForegroundColor Gray }

# git and gh both write to stderr during normal, successful operation, which
# PowerShell turns into a terminating error while ErrorActionPreference is Stop.
# Run them with that relaxed and judge them by exit code instead.
function Native {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$Arguments = @(),
        [switch]$Quiet
    )
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($Quiet) { & $Exe @Arguments *> $null }
        else        { & $Exe @Arguments 2>&1 | ForEach-Object { Write-Host $_ } }
    }
    finally { $ErrorActionPreference = $prev }
    return $LASTEXITCODE
}

$root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
Set-Location $root

# ---------------------------------------------------------------- 1. checks

Step 'Checking tools'
foreach ($tool in @('git', 'gh')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is not installed or not on PATH."
    }
}
if ((Native gh @('auth', 'status') -Quiet) -ne 0) {
    throw 'Not signed in to GitHub. Run: gh auth login'
}

$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$owner = (& gh api user --jq .login 2>$null | Out-String).Trim()
$ErrorActionPreference = $prevEap
if (-not $owner) { throw 'Could not read your GitHub username from gh.' }
Ok "git and gh ready, signed in as $owner"

# ---------------------------------------------------------------- 1b. generated files

Step 'Regenerating every file the docs define'

$utf8 = New-Object System.Text.UTF8Encoding($false)
$fence = [string][char]0x60 * 3

# A doc sits at the root before the first run and under docs\ after it, so this
# has to look in both or a second run silently stops regenerating anything.
function Doc([string]$name) {
    foreach ($p in @((Join-Path $root $name), (Join-Path $root "docs\$name"))) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Grab([string]$doc, [string]$lang, [int]$index = 0) {
    if (-not $doc) { return $null }
    if (-not (Test-Path $doc)) { return $null }
    $text = Get-Content $doc -Raw
    $mm = [regex]::Matches($text, "(?ms)^$fence$lang\r?\n(.*?)^$fence")
    if ($mm.Count -le $index) { return $null }
    return $mm[$index].Groups[1].Value
}

function Emit([string]$path, [string]$body) {
    if (-not $body) { Note "skipped $path - no matching block found"; return }
    $full = Join-Path $root $path
    $dir = Split-Path -Parent $full
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [IO.File]::WriteAllText($full, $body, $utf8)
    Ok $path
}

Emit 'flock_sigs.h'       (Grab (Doc 'FIRMWARE.md') 'cpp' 0)
Emit 'flock-mini.ino'     (Grab (Doc 'FIRMWARE.md') 'cpp' 1)
Emit 'platformio.ini'     (Grab (Doc 'FIRMWARE.md') 'ini' 0)
Emit 'install.ps1'        (Grab (Doc 'INSTALLER.md') 'powershell' 0)
Emit 'flock_installer.py' (Grab (Doc 'GUI_INSTALLER.md') 'python' 0)
Emit 'webflash.ps1'       (Grab (Doc 'WEBFLASH.md') 'powershell' 0)
Emit 'web\index.html'     (Grab (Doc 'WEBFLASH.md') 'html' 0)
Emit 'web\manifest.json'  (Grab (Doc 'WEBFLASH.md') 'json' 0)

Note 'platformio.ini was reset, so this machine COM port is not committed.'

# ---------------------------------------------------------------- 2. build

if ($SkipBuild) {
    Step 'Skipping the build (-SkipBuild)'
}
else {
    Step 'Recompiling to confirm the fixes'
    & python -m platformio run
    if ($LASTEXITCODE -ne 0) { throw 'Build failed - not publishing broken code.' }
    Ok 'Build succeeded'
}

Step 'Staging the browser flasher binary'
$bin = Get-ChildItem '.\.pio\build\*\firmware.bin' -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($bin) {
    Copy-Item $bin.FullName '.\web\firmware.bin' -Force
    Ok ("web/firmware.bin  {0:N0} KB" -f ($bin.Length / 1KB))
    Note 'Committing this means visitors can flash from the hosted page with no toolchain.'
}
else {
    Note 'No firmware.bin found, so the browser flasher will have nothing to serve.'
}

# ---------------------------------------------------------------- 3. layout

Step 'Arranging the repository'

New-Item -ItemType Directory -Force -Path '.\docs' | Out-Null
foreach ($doc in @('START-HERE.md', 'INSTALL.md', 'INSTALLER.md',
                   'GUI_INSTALLER.md', 'WEBFLASH.md', 'PUBLISH.md')) {
    if (Test-Path ".\$doc") {
        Move-Item ".\$doc" ".\docs\$doc" -Force
        Note "docs/$doc"
    }
}

$ignore = @(
    '.pio/', '.vscode/', '.venv/', '__pycache__/', 'build/', 'dist/',
    '*.spec', '*.bak', 'publish.ps1',
    'Install flock-mini.cmd'
) -join "`r`n"
Set-Content '.\.gitignore' $ignore -Encoding ASCII
Ok '.gitignore'

$year = (Get-Date).Year
$license = @(
    'MIT License',
    '',
    "Copyright (c) $year $owner",
    '',
    'Permission is hereby granted, free of charge, to any person obtaining a copy',
    'of this software and associated documentation files (the "Software"), to deal',
    'in the Software without restriction, including without limitation the rights',
    'to use, copy, modify, merge, publish, distribute, sublicense, and/or sell',
    'copies of the Software, and to permit persons to whom the Software is',
    'furnished to do so, subject to the following conditions:',
    '',
    'The above copyright notice and this permission notice shall be included in all',
    'copies or substantial portions of the Software.',
    '',
    'THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR',
    'IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,',
    'FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE',
    'AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER',
    'LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,',
    'OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE',
    'SOFTWARE.'
) -join "`r`n"
Set-Content '.\LICENSE' $license -Encoding ASCII
Ok 'LICENSE (MIT)'

# ---------------------------------------------------------------- 4. git

Step 'Preparing the commit'

if (-not (Test-Path '.\.git')) {
    Native git @('init', '-b', 'main') -Quiet | Out-Null
    Ok 'git repository initialised'
}

# repo-local identity only, so the global config is left alone
if (-not (& git config user.name)) {
    Native git @('config', 'user.name', $owner) -Quiet | Out-Null
}
if (-not (& git config user.email)) {
    Native git @('config', 'user.email', "$owner@users.noreply.github.com") -Quiet | Out-Null
}

Native git @('add', '-A') -Quiet | Out-Null
if ((Native git @('commit', '-m', $Message) -Quiet) -eq 0) { Ok "Committed: $Message" }
else { Note 'Nothing new to commit, continuing.' }

# ---------------------------------------------------------------- 5. publish

Step "Creating github.com/$owner/$RepoName"

$visibility = if ($Private) { '--private' } else { '--public' }

if ((Native gh @('repo', 'view', "$owner/$RepoName") -Quiet) -eq 0) {
    Note 'Repository already exists, pushing to it.'
    Native git @('remote', 'remove', 'origin') -Quiet | Out-Null
    Native git @('remote', 'add', 'origin', "https://github.com/$owner/$RepoName.git") -Quiet | Out-Null
    if ((Native git @('push', '-u', 'origin', 'main')) -ne 0) { throw 'Push failed.' }
}
else {
    $create = @('repo', 'create', $RepoName, $visibility,
                '--source=.', '--remote=origin', '--push', '--description', $Description)
    if ((Native gh $create) -ne 0) { throw 'gh repo create failed.' }
}

Write-Host ''
Ok "https://github.com/$owner/$RepoName"
Note 'Browser flasher: Settings > Pages > deploy from main, folder / (root).'
Note 'Pages only serves from / or /docs, so the flasher page ends up at'
Note "https://$($owner.ToLower()).github.io/$RepoName/web/"
Write-Host ''
```
