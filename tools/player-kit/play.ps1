# Play Grabbed by the Ghoulies (the Windows download's "Play Ghoulies.bat"
# runs this), as the APB and Punch-Out recomps' downloads.
#
# The download holds everything but the game's own code: that is made here,
# from your own disc, the first time (and again after an update): qilin
# lifts default.xbe into C++, the bundled compiler (LLVM-MinGW) compiles it,
# and it's linked with the prebuilt rest into Ghoulies.exe. Then, and every
# time after, it starts the game.
#
#   play.ps1 [-Rebuild] [-BuildOnly] [-ChooseDisc]
#
# -Rebuild builds again even if Ghoulies.exe is up to date; -BuildOnly
# doesn't start the game afterwards; -ChooseDisc asks for the disc folder again.
param(
    [switch]$Rebuild,
    [switch]$BuildOnly,
    [switch]$ChooseDisc
)
$ErrorActionPreference = "Stop"
trap {
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}

$kit = $PSScriptRoot
$top = Split-Path -Parent $kit
$work = Join-Path $top "work"
$game = Join-Path $top "Ghoulies.exe"
$log = Join-Path $top "run.log"
$stamp = Join-Path $work "built-with.txt"
$discFile = Join-Path $work "disc-folder.txt"
$kitVersion = (Get-Content (Join-Path $kit "VERSION") -Raw).Trim()
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows
# Off Windows (testing the download), QILIN_KIT_RUNNER runs the kit's programs.
$runner = $env:QILIN_KIT_RUNNER

# The supported build: the USA release (MS-083), as its certificate and
# timestamp name it. The runtime knows this XBE's addresses, so no other build runs.
$supportedTitle = "4D530053"
$supportedTimestamp = 1064426049

# Runs one of the kit's programs, its output to the window; returns its exit code.
function Invoke-Kit([string]$exe, [string[]]$arguments) {
    if ($runner) { & $runner $exe @arguments | Out-Host } else { & $exe @arguments | Out-Host }
    return $LASTEXITCODE
}
# The same, its output (error output too) returned as text, with the exit code.
function Read-Kit([string]$exe, [string[]]$arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"   # Windows PowerShell 5.1 turns redirected error output into errors
    $text = if ($runner) { & $runner $exe @arguments 2>&1 | Out-String } else { & $exe @arguments 2>&1 | Out-String }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    return @{ text = $text; code = $code }
}

$qilin = Join-Path $kit "qilin.exe"

# Whether a folder is the extracted disc this port supports. Returns "" if it
# is, else why not.
function Test-Disc([string]$folder) {
    $xbe = Join-Path $folder "default.xbe"
    if (-not (Test-Path $xbe)) {
        return "there's no default.xbe in it (extract the whole disc image, for example with extract-xiso or xdvdfs, and choose the folder that holds default.xbe)"
    }
    foreach ($needed in @("xwavebank", "bundles", "demand")) {
        if (-not (Test-Path (Join-Path $folder $needed))) { return "there's no $needed folder in it (extract the whole disc, not just default.xbe)" }
    }
    $report = Read-Kit $qilin @("info", $xbe)
    if ($report.code -ne 0) { return "qilin can't read its default.xbe: $($report.text.Trim())" }
    $info = $report.text | ConvertFrom-Json
    if ($info.certificate.title_id -ne $supportedTitle) { return "its default.xbe is $($info.certificate.title_name) ($($info.certificate.title_id_str)), not Grabbed by the Ghoulies (MS-083)" }
    if ($info.timestamp -ne $supportedTimestamp) { return "its default.xbe isn't the supported build (built at $($info.timestamp)): this port needs the USA release, MS-083" }
    return ""
}

# The disc folder: the one used last, else ask (until one holds the supported build).
$disc = if (-not $ChooseDisc -and (Test-Path $discFile)) { (Get-Content $discFile -Raw).Trim() } else { "" }
while ($true) {
    if ($disc) {
        $why = Test-Disc $disc
        if (-not $why) { break }
        Write-Host "$disc can't be used: $why" -ForegroundColor Yellow
    }
    if (-not $onWindows) { throw "no disc folder: write it into $discFile" }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Choose your extracted Grabbed by the Ghoulies disc: the folder that holds default.xbe"
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        Write-Host "No disc folder chosen."
        exit 0
    }
    $disc = $dialog.SelectedPath
}
$disc = $disc.TrimEnd('\')
New-Item -ItemType Directory -Force -Path $work | Out-Null
Set-Content -Path $discFile -Value $disc -Encoding UTF8

# Starts the game, its messages into run.log (the file to send when something goes wrong).
function Start-Game {
    $xbe = Join-Path $disc "default.xbe"
    if (-not $onWindows) {
        Write-Host "(not on Windows: built, not started)"
        return
    }
    $command = "`"`"$game`" --game-dir `"$disc`" `"$xbe`" > `"$log`" 2>&1`""
    Start-Process -FilePath "cmd.exe" -ArgumentList @("/s", "/c", $command) -WorkingDirectory $top -WindowStyle Hidden
}

# Up to date: just play.
$built = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { "" }
if (-not $Rebuild -and (Test-Path $game) -and $built -eq $kitVersion) {
    if (-not $BuildOnly) { Start-Game }
    exit 0
}

Write-Host "Grabbed by the Ghoulies QilinComp $kitVersion" -ForegroundColor Cyan
if (Test-Path $game) { Write-Host "Updating the game for this version: a few minutes." }
else { Write-Host "Building the game from your disc. This happens once (and after an update): about 5 to 20 minutes, depending on the PC." }
Write-Host "Disc: $disc"
Write-Host ""

# Lift default.xbe (files whose text is unchanged are left alone, so an
# update recompiles only what changed).
Write-Host "== Recompiling default.xbe into C++" -ForegroundColor Cyan
$gen = Join-Path $work "gen"
if ((Invoke-Kit $qilin @("lift", (Join-Path $disc "default.xbe"), "--out", $gen)) -ne 0) { throw "qilin lift failed" }
$failures = Join-Path $gen "failures.txt"
if ((Test-Path $failures) -and (Get-Item $failures).Length -gt 0) { throw "qilin couldn't recompile all of default.xbe (see $failures)" }

# Compile and link: a ninja file over what the lift wrote, with the kit's
# flags and libraries (kit.ninja). Paths are relative to the work folder, so
# spaces in where the download was unpacked don't matter.
$sources = @(Get-ChildItem $gen -Filter *.cpp | Sort-Object Name)
$lines = @(
    "# Written by play.ps1: the game's lifted code, compiled against the kit.",
    "kit = ../kit",
    "include ../kit/kit.ninja",
    "cxx = ..\kit\toolchain\bin\x86_64-w64-mingw32-clang++.exe",
    "rule cxx",
    "  command = `$cxx `$cxxflags -Igen -MD -MF `$out.d -c `$in -o `$out",
    "  depfile = `$out.d",
    "  deps = gcc",
    "  description = Compiling `$in",
    "rule link",
    "  command = `$cxx `$ldflags -o `$out `$in `$libs",
    "  description = Linking `$out"
)
if (-not $onWindows) { $lines[3] = "cxx = " + $env:QILIN_KIT_CXX }   # testing off Windows: a cross compiler
$objects = @()
foreach ($s in $sources) {
    $object = "obj/$($s.BaseName).o"
    $objects += $object
    $lines += "build ${object}: cxx gen/$($s.Name)"
}
$lines += "build Ghoulies.exe: link $($objects -join ' ') | ../kit/lib/libghoulies_kit.a"
$lines += "default Ghoulies.exe"
Set-Content -Path (Join-Path $work "build.ninja") -Value $lines -Encoding ASCII

Write-Host "== Compiling ($($sources.Count) files; the biggest take a few minutes each)" -ForegroundColor Cyan
$ninja = if ($onWindows) { Join-Path $kit "ninja.exe" } else { "ninja" }
$jobs = [Math]::Max(1, [Environment]::ProcessorCount)
if ($onWindows) { $code = Invoke-Kit $ninja @("-C", $work, "-j", "$jobs") }
else { & $ninja -C $work -j $jobs | Out-Host; $code = $LASTEXITCODE }
if ($code -ne 0) { throw "compiling failed" }
Copy-Item (Join-Path $work "Ghoulies.exe") $game -Force
Set-Content -Path $stamp -Value $kitVersion -Encoding ASCII
Write-Host ""
Write-Host "Built: $game" -ForegroundColor Green
if (-not $BuildOnly) {
    Write-Host "Starting Grabbed by the Ghoulies..."
    Start-Game
}
exit 0
