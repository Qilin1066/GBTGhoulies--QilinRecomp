param([string]$Stub, [string]$Launcher, [string]$Out)
$ErrorActionPreference = 'Stop'
# Test metacharacters and names outside the Windows active code page, too.
$fixture = Join-Path $Out ('Kit with spaces ' + [char]0x00e9)
if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $fixture 'kit/lib'), (Join-Path $fixture 'work') -Force | Out-Null
$disc = Join-Path $fixture ('disc %PATH% & ' + [char]0x4fdd + [char]0x5b58)
foreach ($part in @('xwavebank', 'bundles', 'demand')) { New-Item -ItemType Directory -Path (Join-Path $disc $part) -Force | Out-Null }
Set-Content -LiteralPath (Join-Path $disc 'default.xbe') -Value 'not game data'
Copy-Item -LiteralPath $Launcher -Destination (Join-Path $fixture 'kit/play.ps1')
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'kit/qilin.exe')
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'kit/ninja.exe')
Set-Content -LiteralPath (Join-Path $fixture 'kit/VERSION') -Value 'launcher-test'
Set-Content -LiteralPath (Join-Path $fixture 'kit/lib/libghoulies_kit.a') -Value 'fixture'
Set-Content -LiteralPath (Join-Path $fixture 'kit/kit.ninja') -Value 'fixture'
Set-Content -LiteralPath (Join-Path $fixture 'work/disc-folder.txt') -Value $disc -Encoding UTF8
$shellExe = (Get-Process -Id $PID).Path
$launcherPath = Join-Path $fixture 'kit/play.ps1'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0) { throw 'Initial build failed' }
$stamp = Join-Path $fixture 'work/built-with.txt'
$before = Get-Content -LiteralPath $stamp -Raw
Add-Content -LiteralPath (Join-Path $fixture 'kit/lib/libghoulies_kit.a') -Value 'same version, new contents'
& $shellExe -NoProfile -File $launcherPath -BuildOnly
if ($LASTEXITCODE -ne 0 -or (Get-Content -LiteralPath $stamp -Raw) -eq $before) { throw 'Same-version change did not rebuild' }
if (-not (Test-Path -LiteralPath (Join-Path $fixture 'Ghoulies.exe.previous'))) { throw 'Previous executable missing' }
$before = Get-Content -LiteralPath $stamp -Raw
$game = Join-Path $fixture 'Ghoulies.exe'
$hash = (Get-FileHash -LiteralPath $game).Hash
Add-Content -LiteralPath (Join-Path $fixture 'kit/lib/libghoulies_kit.a') -Value 'another update'
$locked = [System.IO.File]::Open($game, 'Open', 'Read', 'Read')
try {
    & $shellExe -NoProfile -File $launcherPath -BuildOnly
    if ($LASTEXITCODE -eq 0) { throw 'Locked executable unexpectedly replaced' }
} finally { $locked.Dispose() }
if ((Get-FileHash -LiteralPath $game).Hash -ne $hash -or (Get-Content -LiteralPath $stamp -Raw) -ne $before) { throw 'Failed update changed executable or stamp' }
# Recover and launch a real child with Unicode, spaces and shell metacharacters.
& $shellExe -NoProfile -File $launcherPath
if ($LASTEXITCODE -ne 0) { throw 'Launch failed' }
$log = Join-Path $fixture 'run.log'
for ($attempt = 0; $attempt -lt 50; ++$attempt) {
    if ((Test-Path -LiteralPath $log) -and (Get-Content -LiteralPath $log -Raw -Encoding UTF8) -match 'launched:') { break }
    Start-Sleep -Milliseconds 100
}
if ((Get-Content -LiteralPath $log -Raw -Encoding UTF8).Trim() -ne "launched: $disc") { throw 'Child arguments were damaged' }
Write-Host 'Launcher rebuild, locked installation and Unicode child arguments passed'
