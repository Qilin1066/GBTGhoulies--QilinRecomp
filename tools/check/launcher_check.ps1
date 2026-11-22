param([string]$Stub, [string]$Ninja, [string]$Launcher, [string]$Out)
$ErrorActionPreference = 'Stop'
# SHA-256 of a file, upper-case hex. Not Get-FileHash: that cmdlet is missing
# when Windows PowerShell inherits a PowerShell 7 module path, as it does
# when started from pwsh (or from CI's shell).
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
# Test metacharacters and names outside the Windows active code page, too.
$fixture = Join-Path $Out ('Kit with spaces ' + [char]0x00e9)
if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $fixture 'kit/lib'), (Join-Path $fixture 'kit/include'), (Join-Path $fixture 'kit/toolchain/bin'), (Join-Path $fixture 'work') -Force | Out-Null
$disc = Join-Path $fixture ('disc %PATH% & ' + [char]0x4fdd + [char]0x5b58)
foreach ($part in @('xwavebank', 'bundles', 'demand')) { New-Item -ItemType Directory -Path (Join-Path $disc $part) -Force | Out-Null }
Set-Content -LiteralPath (Join-Path $disc 'default.xbe') -Value 'not game data'
Copy-Item -LiteralPath $Launcher -Destination (Join-Path $fixture 'kit/play.ps1')
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'kit/qilin.exe')
Copy-Item -LiteralPath $Ninja -Destination (Join-Path $fixture 'kit/ninja.exe')
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'kit/toolchain/bin/x86_64-w64-mingw32-clang++.exe')
Set-Content -LiteralPath (Join-Path $fixture 'kit/include/stub.hpp') -Value 'header1'
Set-Content -LiteralPath (Join-Path $fixture 'kit/VERSION') -Value 'launcher-test'
Set-Content -LiteralPath (Join-Path $fixture 'kit/lib/libghoulies_kit.a') -Value 'fixture'
Set-Content -LiteralPath (Join-Path $fixture 'kit/kit.ninja') -Value "cxxflags =`nldflags =`nlibs ="
Set-Content -LiteralPath (Join-Path $fixture 'work/disc-folder.txt') -Value $disc -Encoding UTF8
$shellExe = (Get-Process -Id $PID).Path
$launcherPath = Join-Path $fixture 'kit/play.ps1'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0) { throw 'Initial build failed' }
$stamp = Join-Path $fixture 'work/built-with.txt'
$before = Get-Content -LiteralPath $stamp -Raw
$game = Join-Path $fixture 'Ghoulies.exe'
$initialHash = (Get-Sha256 $game)
$calls = Join-Path $fixture 'work/tool-calls.txt'
function Compile-Count { return @(Get-Content -LiteralPath $calls | Where-Object { $_ -eq 'compile' }).Count }
$compiles = Compile-Count
if ($compiles -ne 1) { throw 'Initial Ninja build did not compile exactly once' }
$archive = Join-Path $fixture 'kit/lib/libghoulies_kit.a'
Add-Content -LiteralPath $archive -Value 'same version, new contents'
(Get-Item -LiteralPath $archive).LastWriteTimeUtc = [DateTime]'2000-01-01'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0 -or (Get-Content -LiteralPath $stamp -Raw) -eq $before) { throw 'Same-version change did not rebuild' }
if ((Get-Sha256 $game) -eq $initialHash) { throw 'Old archive timestamp hid a changed runtime: stale link' }
if ((Compile-Count) -ne $compiles) { throw 'Runtime-only update unnecessarily recompiled guest code' }
# Header and compiler changes must rebuild objects even with old timestamps.
$header = Join-Path $fixture 'kit/include/stub.hpp'
Set-Content -LiteralPath $header -Value 'header2'
(Get-Item -LiteralPath $header).LastWriteTimeUtc = [DateTime]'2000-01-01'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0 -or (Compile-Count) -ne ($compiles + 1)) { throw 'Changed header with old timestamp did not compile' }
$compiles = Compile-Count
$compiler = Join-Path $fixture 'kit/toolchain/bin/x86_64-w64-mingw32-clang++.exe'
Add-Content -LiteralPath $compiler -Value 'compiler-overlay'
(Get-Item -LiteralPath $compiler).LastWriteTimeUtc = [DateTime]'2000-01-01'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0 -or (Compile-Count) -ne ($compiles + 1)) { throw 'Changed compiler with old timestamp did not compile' }
$compiles = Compile-Count
# Same certificate/timestamp, changed executable content: rebuild, not cache reuse.
Add-Content -LiteralPath (Join-Path $disc 'default.xbe') -Value 'changed disc code'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0 -or (Compile-Count) -ne ($compiles + 1)) { throw 'Changed disc code did not rebuild' }
$compiles = Compile-Count
$callCount = @(Get-Content -LiteralPath $calls).Count
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0 -or @(Get-Content -LiteralPath $calls).Count -ne $callCount) { throw 'Unchanged build was not reused' }
& $shellExe -NoProfile -File $launcherPath -BuildOnly -Rebuild
if ($LASTEXITCODE -ne 0 -or (Compile-Count) -ne ($compiles + 1)) { throw 'Explicit rebuild did not recompile' }
if (-not (Test-Path -LiteralPath (Join-Path $fixture 'Ghoulies.exe.previous'))) { throw 'Previous executable missing' }
$before = Get-Content -LiteralPath $stamp -Raw
$game = Join-Path $fixture 'Ghoulies.exe'
$hash = (Get-Sha256 $game)
Add-Content -LiteralPath (Join-Path $fixture 'kit/lib/libghoulies_kit.a') -Value 'another update'
$locked = [System.IO.File]::Open($game, 'Open', 'Read', 'Read')
try {
    & $shellExe -NoProfile -File $launcherPath -BuildOnly
    if ($LASTEXITCODE -eq 0) { throw 'Locked executable unexpectedly replaced' }
} finally { $locked.Dispose() }
if ((Get-Sha256 $game) -ne $hash -or (Get-Content -LiteralPath $stamp -Raw) -ne $before) { throw 'Failed update changed executable or stamp' }
# Recover and launch a real child with Unicode, spaces and shell metacharacters.
& $shellExe -NoProfile -File $launcherPath
if ($LASTEXITCODE -ne 0) { throw 'Launch failed' }
$log = Join-Path $fixture 'run.log'
for ($attempt = 0; $attempt -lt 50; ++$attempt) {
    if ((Test-Path -LiteralPath $log) -and (Get-Content -LiteralPath $log -Raw -Encoding UTF8) -match 'launched:') { break }
    Start-Sleep -Milliseconds 100
}
if ((Get-Content -LiteralPath $log -Raw -Encoding UTF8).Trim() -ne "launched: $disc") { throw 'Child arguments were damaged' }
Write-Host 'Real Ninja: old timestamps, headers/compiler/disc changes, explicit rebuild, locked installation and Unicode arguments passed'
