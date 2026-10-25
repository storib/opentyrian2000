# AGENTS.md

Guide for agents (and people) working on this fork. Read it before changing
code. The multiplayer design lives in [doc/multiplayer.md](doc/multiplayer.md),
and the sync tests in [tests/sync/README.md](tests/sync/README.md).

## What this is

A fork of [OpenTyrian2000](https://github.com/KScl/opentyrian2000) (C99 + SDL2,
a port of the DOS shoot-'em-up Tyrian 2000). Goals:

- **Online multiplayer, in the game's own menus.** Target: **4 players on two
  computers, 2 per computer**. The computers can be any mix of **Windows and
  macOS**.
- **At most 2 players per computer, ever.** Four players on one computer is
  deliberately not supported, not even as a test mode.
- **Widescreen**, including the owner's 5120×1440 (32:9) monitor.
- **Later:** new ship types (for now players 3–4 fly duplicates of the
  existing ships).

## Status

| Milestone | State |
|-----------|-------|
| M0 Infrastructure | ✅ Fork, MSYS2 build, CI on Windows/Linux/macOS |
| Widescreen | ✅ Sharp scaling; in-game, playfield centered with the HUD panel moved beside it |
| M1 Input layer + determinism tools | ✅ Adversarially reviewed in 3 rounds; all findings fixed |
| M2 4-player core | Next. Not started |
| M3 Netcode v2 (LAN, Host/Join in menus) | Not started |
| M4 Internet relay (DigitalOcean droplet, room codes) | Not started |

**Known gaps:**
- No sync recording yet exercises orbiting sidekicks, special weapons,
  Nort-ship sparks or random explosions.
- `Player.cash` is `ulong`, so its width differs between Windows and macOS.
  It only matters if cash goes negative, and no game data was found that
  makes it do so.
- The attract-mode demo can still be ended by a real keypress, which is
  harmless for sync.

## Repository

- **Remotes:** `origin` is the fork (`storib/opentyrian2000`); `upstream` is
  `KScl/opentyrian2000`. `master` tracks upstream unchanged.
- **Stacked branches**, each built on the previous one:
  - `multiplayer/design`: the design doc.
  - `widescreen-hud`: dev scripts, widescreen.
  - `m1-sync`: everything since, and the current tip.
- **Untracked local files:** `data/` (the game data), `*.dll` and the built
  exe are git-ignored. The game writes `opentyrian.cfg`, `tyrian.cfg` and
  `tyrian.sav` into its working directory, also ignored.

### Files we added or changed most

| File | What |
|------|------|
| `src/sync.c/.h` | `PlayerInput`, state hash, replay record/replay, game events, state dumps |
| `src/mainint.c` | `read_player_input()` / `apply_player_input()` split out of `JE_playerMovement()` |
| `src/devscript.c/.h` | `--dev-script`: scripted keys, mouse and screenshots |
| `src/dmath.c/.h` | `dm_sinf` / `dm_cosf`: deterministic trig for the simulation |
| `src/video.c/.h` | Widescreen mode, sharp scaling, `video_hud_split` |
| `src/tyrian2.c` | Level loop hooks (`sync_*`), `--quick-arcade` (`quickStartArcade`, `setupNewGame`) |
| `tests/sync/` | Recordings, record/replay scripts |
| `.github/workflows/sync-check.yml` | CI: build on 3 platforms, replay every recording |

## Building

**Windows:** MSYS2 UCRT64 (installed at `C:\msys64`; needs `make`, `gcc`,
`pkgconf`, `SDL2`, `SDL2_net`). From PowerShell in the repo:

```
$env:MSYSTEM='UCRT64'; $env:CHERE_INVOKING='1'; & C:\msys64\usr\bin\bash.exe -lc 'make -j8'
```

Then copy `SDL2.dll` and `SDL2_net.dll` from `C:\msys64\ucrt64\bin` next to
the exe if they aren't already there.

- **Data:** the freeware Tyrian 2000 data goes in `data/`, from
  `https://www.camanis.net/tyrian/tyrian2000.zip`.
- **clang:** a clang toolchain (MSYSTEM=CLANG64, `make CC=clang
  VCS_IDREV=true`) reproduces most macOS compiler behaviour on Windows. Build
  it in a copy of the repo, not in place.
- **Linux/macOS:** see the CI workflow (apt `libsdl2-dev libsdl2-net-dev`;
  brew `sdl2 sdl2_net pkg-config`), then `make`.
- **Compiler warnings:** the gcc build is warning-free with `-Wall -Wextra
  -pedantic`. Keep it that way. clang shows a few upstream "set but not used"
  warnings.
- **Makefile flags:** don't remove `-ffp-contract=off` (see determinism
  below).

## Dev tools (command line)

| Option | Purpose |
|--------|---------|
| `--dev-script=FILE` | Drive the game from a script. Commands: `wait`, `key`, `down`/`up`, `mouse`, `mousedown`/`mouseup`, `shot`, `window`, `mode`, `quit [code]`. See `src/devscript.h`. Real input is ignored while a script runs, and the game doesn't auto-pause. |
| `--quick-arcade=P:E:D` | Start a P-player arcade game (episode E, `easy`/`normal`/`hard`) with no menus. Required for recording and replaying. |
| `--record-input=FILE` | Record input, game events and per-tick hashes. Written to `FILE.part`, moved over `FILE` only on a clean quit. |
| `--replay-input=FILE` | Replay and verify every tick. |
| `--hash-log=FILE` | Write each tick's state hash. |
| `--state-dump=FILE` (+ `--state-dump-tick=N`) | Write every hashed value by name, on divergence or at tick N. Diff two machines' dumps to find what differs. |

**Replay exit codes:**

| Code | Meaning |
|------|---------|
| 0 | Every tick verified (only ever means that) |
| 3 | Diverged |
| 4 | Script timeout |
| 5 | Truncated, corrupt or empty recording |
| 6 | Quit before the replay finished |

**Screenshots:** `shot` saves the final window output as BMP, which works
headless (`SDL_VIDEODRIVER=dummy`). That's how display changes are checked
without anyone at the screen. Don't send keystrokes to the desktop instead:
Windows won't focus a background window, so they land in whatever else has
focus.

## Determinism rules (must follow)

Online play is lockstep: every computer runs the whole simulation from the
same inputs, so every computer must compute **bit-identical** results, across
Windows/macOS, x86/ARM and gcc/clang. Any change to simulation code must keep
these rules:

1. **Input only enters through `PlayerInput`** (`read_player_input`) or
   `sync_event()`. Anything else a player does that changes the game must be
   recorded as an event, or disabled while `sync_is_deterministic()`.
2. **No libm trig in simulation code.** Use `dm_sinf` / `dm_cosf`. `sqrtf`,
   `roundf` and basic arithmetic are fine. Visual-only code (starfield, menus,
   audio) may use libm.
3. **One side effect per expression.** Never write `f(mt_rand(), mt_rand())`
   or `mt_rand() % a + mt_rand() % b`: gcc and clang evaluate them in
   opposite orders. This caused a real PC/Mac desync. Draw into named locals,
   one per statement.
4. **No wall-clock time, local settings or uninitialised values** in
   simulation code. Settings that change gameplay must be identical on every
   machine.
5. **Watch types whose size differs between platforms**: `long` is 32-bit on
   Windows and 64-bit on macOS. Watch signed/unsigned mixes too, such as
   dividing by `COUNTOF()` (a `size_t`).
6. **Hash new simulation state.** Add it to `sync_state_hash()`, by value with
   `H()`, never raw memory: structs hold pointers and padding. Pointers into
   map data are hashed as offsets.
7. **Re-record and re-verify.** Any deliberate gameplay change invalidates
   the recordings; re-record from default settings (see
   tests/sync/README.md). Any accidental change shows up as a divergence.

To debug a divergence: run the replay with `--state-dump=FILE` on both
machines (CI uploads its dumps as artifacts), then diff the dumps. The first
differing line names the value. A clang build on Windows usually reproduces
macOS differences locally.

## Testing

- **CI** replays every `tests/sync/*.otr` on Windows, Linux and macOS (Apple
  Silicon). It uploads screenshots, hash logs and state dumps.
- **Locally:** replay with `SDL_VIDEODRIVER=dummy`, and delete the
  cfg/sav files first so runs start from defaults:
  ```
  opentyrian2000 --no-sound --quick-arcade=2:1:normal --dev-script=tests/sync/2p-arcade-replay.txt --replay-input=tests/sync/2p-arcade.otr
  ```
- **The regression battery isn't in the repo yet.** The review fixes were
  checked with a 43-check PowerShell script kept outside it (truncated files,
  exit codes, F1/menu/quit recordings, failed saves, window close, option
  parsing). Adding it under `tests/` is worthwhile.
- **Review history:** an adversarial review agent swept all changes since
  upstream in three rounds (18 findings, all fixed). The commit messages from
  `23a0509` on describe each fix.

## Environment gotchas (Windows dev machine)

- **The shell is Windows PowerShell 5.1.** Double quotes inside arguments to
  native programs get mangled:
  - Single-quote bash commands passed to MSYS2.
  - Commit with `git commit -F <file>` whenever the message contains double
    quotes.
- **PATH:** git and gh may not be on PATH in an old shell. Refresh it with
  `$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')`.
- **Exit codes:** Windows GUI-subsystem builds (`-mwindows`) don't wait in
  PowerShell. Use `Start-Process -PassThru`, read `$p.Handle` right away (so
  `ExitCode` works later), and redirect stderr to a file.
- **OneDrive:** the repo sits in a OneDrive-synced folder. Sync can hold file
  handles; recordings use an atomic replace for that reason.
- **Focus pausing:** the game pauses whenever its window loses focus, except
  under `--dev-script`.

## Conventions

- **Code style:** match the surrounding code: tabs, C99, `JE_`-style names in
  old code, short comments that explain *why*. New files get the GPLv2 header
  used everywhere else.
- **Commits:** small and descriptive; the body explains the bug or the
  reason. End with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  Commit identity is repo-local, with the owner's GitHub no-reply email.
- **Scope:** don't add features beyond what was asked. Ask the owner before
  changing gameplay or the design decisions listed above.
