# Sync tests

Online play is deterministic lockstep: every machine runs the whole game from
the same inputs, so every machine must compute exactly the same result. These
tests check that.

`2p-arcade.otr` is a recording of about 19 seconds of 2 Player Arcade (episode
1, normal). It holds each player's input for every tick, plus a hash of the
simulation state at the end of every tick. Replaying it re-runs the game from
the recorded input and checks every hash:

```
opentyrian2000 --quick-arcade=2:1:normal --dev-script=tests/sync/2p-arcade-replay.txt --replay-input=tests/sync/2p-arcade.otr
```

`--quick-arcade` starts the game directly, without the menus, so the test
doesn't depend on how fast a machine gets through them.

The game exits by itself when the replay ends:

- exit code 0 and `replay: finished, N ticks verified`, or
- exit code 3 and `replay: DIVERGED at level L tick T: ...`, naming the first
  tick where the state differs, or
- exit code 4 if the replay got stuck and the script's timeout was reached.

CI runs this on Windows, Linux and macOS (`.github/workflows/sync-check.yml`).
It uploads screenshots and the per-tick hash log (`--hash-log`) for each
platform.

## Re-recording

A change that deliberately alters gameplay invalidates the recording. To make a
new one, start from default settings (delete `opentyrian.cfg`, `tyrian.cfg` and
`tyrian.sav` from the game folder), then:

```
opentyrian2000 --quick-arcade=2:1:normal --dev-script=tests/sync/2p-arcade-record.txt --record-input=tests/sync/2p-arcade.otr
```
