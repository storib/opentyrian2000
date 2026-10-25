# Network game tests on one Windows computer: two game instances play each other,
# headless, driven by dev scripts.
#
#   powershell -File tests/net/run-local.ps1 [-Mode MODE] [-Relay HOST[:PORT]]
#
# Modes:
#   internet  (default) host and join with a room code through a relay; pause,
#             go on, and quit from the in-game menu. Both players must compute
#             the same state hash on every tick.
#   lan       the same, but player 1 hosts on this computer and player 2 finds
#             the game with LAN discovery.
#   desync    player 2's game is changed on purpose at tick 200; both players
#             must stop, report the desync and write state dumps that differ.
#   leave     player 2 closes the game mid-level; player 1 must be told.
#   menu-leave  player 2 closes the game while its in-game menu holds both
#             players; player 1 must not be left waiting.
#
# Without -Relay, a relay is started here (every mode but lan). -Relay default
# uses the game's built-in relay address, as a player's game would.
# Run from the repository root after building opentyrian2000.exe and
# tyrian-relay.exe. Exits 0 if the test passed.
param(
    [ValidateSet('internet', 'lan', 'desync', 'leave', 'menu-leave')][string]$Mode = 'internet',
    [string]$Relay = ''
)

$ErrorActionPreference = 'Stop'
$out = 'tests\net\out'
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -File | Remove-Item
foreach ($f in @('opentyrian.cfg', 'tyrian.cfg', 'tyrian.sav') + (Get-ChildItem 'desync-tick-*.txt' | ForEach-Object Name)) {
    if (Test-Path $f) { Remove-Item $f }
}

$env:SDL_VIDEODRIVER = 'dummy'
$env:SDL_AUDIODRIVER = 'dummy'

# the scripts each player runs; LAN mode swaps the marked menu choices
$hostScript = if ($Mode -eq 'internet' -or $Mode -eq 'lan') { 'host' } else { 'host-play' }
$joinScript = switch ($Mode) { 'leave' { 'join-quit' } 'menu-leave' { 'join-menu-quit' } default { 'join' } }
foreach ($s in @($hostScript, $joinScript)) { Copy-Item "tests\net\$s.txt" "$out\$s.txt" }
if ($Mode -eq 'lan') {
    $marked = '(?s)# >>>.*?# <<<'
    (Get-Content "$out\host.txt" -Raw) -replace $marked, "key Down`nkey Down`nkey Return" | Set-Content "$out\host.txt" -Encoding ascii
    (Get-Content "$out\join.txt" -Raw) -replace $marked, "key Down`nkey Down`nkey Down`nkey Return`nwait 3000`nkey Return" | Set-Content "$out\join.txt" -Encoding ascii
}

$relayProcess = $null
if ($Relay -eq '' -and $Mode -ne 'lan') {
    $relayProcess = Start-Process .\tyrian-relay.exe -ArgumentList '--port=21999' -PassThru -NoNewWindow -RedirectStandardError "$out\relay.txt" -RedirectStandardOutput "$out\relay-out.txt"
    $Relay = '127.0.0.1:21999'
    Start-Sleep -Milliseconds 500
}

function Start-Player($name, $script) {
    $arguments = @('--no-sound', "--net-name=$name", "--dev-script=$out\$script.txt", "--hash-log=$out\$name-hash.txt")
    if ($Relay -ne '' -and $Relay -ne 'default') { $arguments += "--relay=$Relay" }
    $p = Start-Process .\opentyrian2000.exe -ArgumentList $arguments -PassThru -NoNewWindow -RedirectStandardError "$out\$name-stderr.txt" -RedirectStandardOutput "$out\$name-stdout.txt"
    $null = $p.Handle  # keeps ExitCode readable after the process ends
    return $p
}

$hostPlayer = Start-Player 'HOST' $hostScript
if ($Mode -eq 'desync') { $env:OPENTYRIAN_TEST_DESYNC_TICK = '200' }
$joinPlayer = Start-Player 'GUEST' $joinScript
Remove-Item Env:\OPENTYRIAN_TEST_DESYNC_TICK -ErrorAction SilentlyContinue

# hand the room code to player 2, which types it from code.txt
$code = $null
for ($i = 0; $i -lt 300 -and $code -eq $null; $i++) {
    Start-Sleep -Milliseconds 100
    $line = Get-Content "$out\HOST-stderr.txt" -ErrorAction SilentlyContinue | Select-String 'net: in room (\w+)' | Select-Object -First 1
    if ($line) { $code = $line.Matches[0].Groups[1].Value }
}
if ($code -eq $null) { $code = 'NONE' }
Set-Content -Path "$out\code.txt" -Value $code -Encoding ascii
Write-Output "$Mode test, room code $code"

$hostPlayer.WaitForExit(180000) | Out-Null
$joinPlayer.WaitForExit(180000) | Out-Null
if ($relayProcess) { Stop-Process -Id $relayProcess.Id -ErrorAction SilentlyContinue }

$failed = $false
function Fail($message) { Write-Output "FAIL: $message"; $script:failed = $true }

$logs = @{}
foreach ($p in @(@('HOST', $hostPlayer), @('GUEST', $joinPlayer))) {
    if (-not $p[1].HasExited) { Stop-Process -Id $p[1].Id; Fail "$($p[0]) didn't finish" }
    elseif ($p[1].ExitCode -ne 0) { Fail "$($p[0]) exited with $($p[1].ExitCode)" }
    $logs[$p[0]] = Get-Content "$out\$($p[0])-stderr.txt"
    $logs[$p[0]] | Select-String '^net:' | ForEach-Object { "  $($p[0]) $_" }
}

function Expect($player, $text) {
    if (-not ($logs[$player] | Select-String $text -SimpleMatch -Quiet)) { Fail "$player never logged '$text'" }
}

$hostHashes = Get-Content "$out\HOST-hash.txt"
$joinHashes = Get-Content "$out\GUEST-hash.txt"
$ticks = ($hostHashes | Where-Object { $_ -notmatch '^L ' }).Count

switch ($Mode) {
    { $_ -eq 'internet' -or $_ -eq 'lan' } {
        foreach ($player in 'HOST', 'GUEST') {
            foreach ($event in 'game started', 'paused at tick', 'HOST went on at tick', 'HOST opened the menu at tick', 'HOST ended the game at tick') {
                Expect $player $event
            }
        }
        if ($ticks -lt 200) { Fail "only $ticks ticks were played" }
        if (Compare-Object $hostHashes $joinHashes -SyncWindow 0) { Fail 'the hash logs differ' }
        if ($relayProcess -and (Select-String -Path "$out\relay.txt" -Pattern 'DESYNC' -Quiet)) { Fail 'the relay saw a desync' }
    }
    'desync' {
        Expect 'HOST' 'DESYNC at tick 200;'
        Expect 'GUEST' 'DESYNC at tick 200;'
        $dumps = @(Get-ChildItem 'desync-tick-*-player*.txt')
        if ($dumps.Count -ne 2) { Fail "expected 2 desync dumps, found $($dumps.Count)" }
        else {
            $difference = Compare-Object (Get-Content $dumps[0]) (Get-Content $dumps[1]) | Where-Object { $_.InputObject -notmatch '^hash ' }
            if (-not ($difference | Where-Object { $_.InputObject -match 'cash' })) { Fail 'the dumps should differ in cash' }
            $difference | ForEach-Object { "  dump difference: $($_.InputObject)" }
            $dumps | Move-Item -Destination $out
        }
    }
    'leave' {
        Expect 'HOST' 'game over after'
        Expect 'HOST' 'GUEST left the game.'
    }
    'menu-leave' {
        Expect 'HOST' 'GUEST opened the menu at tick'
        Expect 'HOST' 'GUEST left the game.'
    }
}

if ($failed) { Write-Output "$Mode test FAILED"; exit 1 }
Write-Output "$Mode test passed ($ticks ticks)"
