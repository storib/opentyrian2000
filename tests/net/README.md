# Network tests

`run-local.ps1` runs two game instances on one Windows computer, headless, and
has them play a network game against each other. Dev scripts drive both, so
nobody needs to be at the keyboard. (Two instances on one computer are two
computers as far as the game is concerned; each still has one player.)

```
powershell -File tests/net/run-local.ps1 -Mode internet
```

Build `opentyrian2000.exe` and `tyrian-relay.exe` (`make relay`) first, with
`SDL2.dll` and `libenet-7.dll` next to them. Without `-Relay HOST[:PORT]` the
script starts a relay on this computer, on UDP port 21999.

| Mode | Scripts | Passes when |
|------|---------|-------------|
| `internet` | `host.txt`, `join.txt` | Player 1 hosts with a room code, player 2 joins with it, they play, player 1 pauses and goes on, then quits from the in-game menu. Both log every pause and the quit at the same tick, and their per-tick state hashes (`--hash-log`) are identical. |
| `lan` | the same, with the marked menu choices swapped | As above, but player 1 hosts on this computer and player 2 finds the game with LAN discovery. |
| `desync` | `host-play.txt`, `join.txt` | Player 2's game is changed on purpose at tick 200 (`OPENTYRIAN_TEST_DESYNC_TICK`). Both stop at the same tick, report a desync at tick 200, and write `desync-tick-*-player*.txt` dumps that differ in `cash`. |
| `leave` | `host-play.txt`, `join-quit.txt` | Player 2 closes the game mid-level; player 1 is told and returns to the lobby. |
| `menu-leave` | `host-play.txt`, `join-menu-quit.txt` | Player 2 opens the in-game menu, which holds both players, then closes the game; player 1 isn't left waiting behind the menu. |

The room code reaches player 2 through `out/code.txt`: the script reads it
from player 1's log and player 2's script types it with `text @FILE`.

Output goes to `tests/net/out/`: each player's log, hash log and screenshots,
and the relay's log. CI runs every mode but `lan` on
Windows (`.github/workflows/sync-check.yml`) and uploads that folder.

Running the internet test against the real relay:

```
powershell -File tests/net/run-local.ps1 -Mode internet -Relay 206.189.70.45
```

Two separate computers are the real test, which these don't replace: on each,
choose Start New Game > Network Game, host on one and join with its room code
on the other.
