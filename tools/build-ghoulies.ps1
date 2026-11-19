# Builds the game in one go from the source, as the APB and Punch-Out
# recomps' build scripts: the recompiler, the lift of your disc's
# default.xbe, then the game. Run it again after a `git pull`: only what
# changed is rebuilt (the lift leaves unchanged files alone).
#
#   powershell -ExecutionPolicy Bypass -File tools\build-ghoulies.ps1 [-Disc <folder>] [-Run] [-Pull] [-ChooseDisc] [-Play]
#
# On Windows, "Play Ghoulies.bat" and "Update Ghoulies.bat" in the project's
# folder run it for you: double-click them. It needs Visual Studio 2022
# 17.10 or newer (with "Desktop development with C++") and finds its CMake
# and Git itself, from any window. With no -Disc it uses the folder it used
# last (build-game\disc-folder.txt), else asks with a folder picker: the
# extracted disc, the folder holding default.xbe. -Run starts the game after;
# -Pull fetches the latest code first; -Play starts it straight away if it's
# already built. The game's messages go to build-game\run.log.
param(
    [string]$Disc = "",
    [switch]$Run,
    [switch]$Pull,
    [switch]$ChooseDisc,
    [switch]$Play
)
$ErrorActionPreference = "Stop"
trap {
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build-game"
$gen = Join-Path $build "gen"
$discFile = Join-Path $build "disc-folder.txt"
$log = Join-Path $build "run.log"
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows
$exe = if ($onWindows) { ".exe" } else { "" }

function Find-Built([string]$name) {
    @((Join-Path $build "Release/$name$exe"), (Join-Path $build "$name$exe")) | Where-Object { Test-Path $_ } | Select-Object -First 1
}

function Start-Game([string]$game, [string]$disc) {
    $xbe = Join-Path $disc "default.xbe"
    if ($onWindows) {
        $command = "`"`"$game`" --game-dir `"$disc`" `"$xbe`" > `"$log`" 2>&1`""
        Start-Process -FilePath "cmd.exe" -ArgumentList @("/s", "/c", $command) -WorkingDirectory (Split-Path -Parent $game) -WindowStyle Hidden
    } else {
        Start-Process -FilePath $game -ArgumentList @("--game-dir", $disc, $xbe) -RedirectStandardOutput $log
    }
    Write-Host "The game's messages go to $log"
}

if ($Play) {
    $built = Find-Built "qilin_run"
    if ($built -and (Test-Path $discFile)) {
        Start-Game $built (Get-Content $discFile -Raw).Trim()
        exit 0
    }
    Write-Host "Grabbed by the Ghoulies isn't built yet: building it now. The first build takes 10 to 40 minutes."
    $Run = $true
}

function Invoke-Step([string]$what, [string]$program, [string[]]$arguments) {
    Write-Host "== $what" -ForegroundColor Cyan
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

# CMake and Git: on the PATH, else the ones Visual Studio installs, else the usual places.
function Add-Tool([string]$name, [string[]]$folders) {
    if (Get-Command $name -ErrorAction SilentlyContinue) { return $true }
    foreach ($f in $folders) {
        if ($f -and (Test-Path (Join-Path $f "$name.exe"))) {
            $env:PATH = "$f;$env:PATH"
            return $true
        }
    }
    return $false
}
if ($onWindows) {
    $vs = ""
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -property installationPath | Select-Object -First 1 }
    $ide = if ($vs) { Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft" } else { "" }
    $haveCmake = Add-Tool "cmake" @($(if ($ide) { Join-Path $ide "CMake\CMake\bin" }), (Join-Path $env:ProgramFiles "CMake\bin"))
    $haveGit = Add-Tool "git" @((Join-Path $env:ProgramFiles "Git\cmd"), $(if ($ide) { Join-Path $ide "TeamFoundation\Team Explorer\Git\cmd" }))
    if (-not $vs) { throw "Visual Studio 2022 wasn't found. Install it (the free Community edition) with 'Desktop development with C++'." }
    if (-not $haveCmake) { throw "CMake wasn't found. In the Visual Studio Installer, add 'C++ CMake tools for Windows'." }
    if (-not $haveGit) { throw "Git wasn't found. Install Git for Windows (git-scm.com), or add 'Git for Windows' in the Visual Studio Installer." }
} elseif (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake isn't installed."
}

if ($Pull) {
    Write-Host "== Getting the latest code" -ForegroundColor Cyan
    & git -C $root pull --ff-only
    if ($LASTEXITCODE -ne 0) { throw "git pull failed (exit code $LASTEXITCODE). If you changed files in the project folder, that's why." }
}

# The extracted disc: the one used last, else ask.
if (-not $Disc -and -not $ChooseDisc -and (Test-Path $discFile)) { $Disc = (Get-Content $discFile -Raw).Trim() }
while (-not $Disc -or -not (Test-Path (Join-Path $Disc "default.xbe"))) {
    if ($Disc) { Write-Host "$Disc has no default.xbe: choose the extracted disc's folder (it holds default.xbe, bundles, xwavebank and the rest)." -ForegroundColor Yellow }
    if (-not $onWindows) { throw "give the extracted disc with -Disc <folder>" }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Choose your extracted Grabbed by the Ghoulies disc: the folder that holds default.xbe"
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw "no disc folder chosen" }
    $Disc = $dialog.SelectedPath
}
$Disc = $Disc.TrimEnd('\')
$xbe = Join-Path $Disc "default.xbe"

# The recompiler first, then the lift, then the game from the lifted code.
$configure = @("-S", $root, "-B", $build, "-DQILIN_GEN_DIR=$gen")
if ($onWindows -and -not (Test-Path (Join-Path $build "CMakeCache.txt"))) { $configure += @("-G", "Visual Studio 17 2022", "-A", "x64") }
if (-not $onWindows) { $configure += "-DCMAKE_BUILD_TYPE=Release" }
New-Item -ItemType Directory -Force -Path $gen | Out-Null
Invoke-Step "Configuring" "cmake" $configure
Set-Content -Path $discFile -Value $Disc -Encoding UTF8
Invoke-Step "Building the recompiler" "cmake" @("--build", $build, "--config", "Release", "--target", "qilin", "--parallel")
$qilin = Find-Built "qilin"
if (-not $qilin) { throw "built, but qilin$exe isn't where it should be" }
Invoke-Step "Recompiling default.xbe into C++" $qilin @("lift", $xbe, "--out", $gen)
Invoke-Step "Configuring with the recompiled code" "cmake" @("-S", $root, "-B", $build)
Invoke-Step "Building Grabbed by the Ghoulies (the first time compiles the whole game: 10 to 40 minutes)" "cmake" @("--build", $build, "--config", "Release", "--target", "qilin_run", "--parallel")
$game = Find-Built "qilin_run"
if (-not $game) { throw "built, but qilin_run$exe isn't where it should be" }

Write-Host "Built: $game" -ForegroundColor Green
if ($Run) {
    Write-Host "Starting Grabbed by the Ghoulies..."
    Start-Game $game $Disc
} else {
    Write-Host "Run it with:  & `"$game`" --game-dir `"$Disc`" `"$xbe`""
}
