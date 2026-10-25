# Sync tests

Online play is deterministic lockstep: every machine runs the whole game from
the same inputs, so every machine must compute exactly the same result. These
tests check that.

Each `.otr` file is a recording of 2 Player Arcade (episode 1, normal). It
holds each player's input for every tick, a few game events that don't come
from player input (the in-game menu, the game over screen), and a hash of the
simulation state at the end of every tick:

| Recording | Covers |
|-----------|--------|
| `2p-arcade.otr` | ~19 s of both players moving and shooting |
| `2p-link.otr` | player 2 (mouse) links onto player 1, then sweeps the Dragonwing's link gun all the way round while firing: the turret maths (`dm_sinf`/`dm_cosf`) and aimed shots |

Replaying re-runs the game from the recorded input and checks every hash:

```
opentyrian2000 --quick-arcade=2:1:normal --dev-script=tests/sync/2p-arcade-replay.txt --replay-input=tests/sync/2p-arcade.otr
```

`--quick-arcade` starts the game directly, without the menus, so the test
doesn't depend on how fast a machine gets through them. While recording or
replaying, help and cheat keys are ignored, because they would change the game
without being recorded.

The game exits by itself when the replay ends:

- exit code 0 and `replay: finished, N ticks verified`, or
- exit code 3 and `replay: DIVERGED at level L tick T: ...`, naming the first
  tick where the state differs (add `--state-dump=FILE` to write every hashed
  value at that tick, for diffing against another machine), or
- exit code 4 if the replay got stuck and the script's timeout was reached, or
- exit code 5 if the recording is truncated, corrupt or empty, or
- exit code 6 if the game quit before the replay finished (a script `quit`,
  closing the window, Ctrl+C).

Exit code 0 always means every recorded tick was verified.

CI replays every `.otr` here on Windows, Linux and macOS
(`.github/workflows/sync-check.yml`). It uploads screenshots, per-tick hash
logs and state dumps for each platform.

## Recording

A change that deliberately alters gameplay invalidates the recordings. To
re-record, start from default settings (delete `opentyrian.cfg`, `tyrian.cfg`
and `tyrian.sav` from the game folder), then for each recording:

```
opentyrian2000 --quick-arcade=2:1:normal --dev-script=tests/sync/2p-arcade-record.txt --record-input=tests/sync/2p-arcade.otr
opentyrian2000 --quick-arcade=2:1:normal --dev-script=tests/sync/2p-link-record.txt --record-input=tests/sync/2p-link.otr
```

The recording is written to `FILE.part` and only moved over `FILE` when the
run quits cleanly (a script `quit` or `quit 0`), so a failed or aborted run
never replaces a good recording. Recording and replaying need
`--quick-arcade`, because choices in the between-level menus aren't recorded;
while recording, that menu is skipped even after Quit Level or a game over.

While a dev script runs, real keyboard, mouse and gamepad input is ignored.
