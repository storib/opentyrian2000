# Multiplayer v2 — design

Status: draft / proposal. Nothing here is implemented yet.

## Goals

- **4 players on two computers, 2 per computer**, as the main target. The two
  computers can be any mix of **Windows and macOS** (Linux comes for free).
  Other splits (4 × 1, 2 + 1 + 1) also work.
- **At most 2 players per computer, always.** Four players sharing one
  computer is deliberately not supported, not even as a test mode: too many
  people on one keyboard and screen gets messy. (Decided 2026-10-24.)
- **Start from inside the game.** Host / Join with a short room code. No IP
  addresses, port forwarding, or command-line flags.
- **Reliable.** If the games ever disagree, both players are told exactly when
  ("desync at tick 4123") instead of silently drifting apart.
- **Ships:** players 3 and 4 reuse existing ships for now. New ship types come
  later.

## What exists today

- `src/network.c`: 2-player, peer-to-peer UDP over SDL2_net with
  hand-rolled acks, resends and XOR redundancy packets. Its own header says
  "HERE BE DRAGONS".
- **Deterministic lockstep.** Each side sends its processed input deltas
  (mouse delta, acceleration, buttons) every frame from inside
  `JE_playerMovement()` (`mainint.c`). Both sides simulate everything. RNG is
  seeded with a fixed value at level start (`tyrian2.c`, `mt_srand(32402394)`).
  Wall-clock time (`SDL_GetTicks`) is only used for debug/idle timers, not
  game logic, so the simulation is deterministic in principle.
- **Sync checking:** only player x/y positions are compared.
- **Launch:** command line only (`--net HOST`, `--net-player-number`), both
  players at the same time, hole punching only. The README says it was never
  tested for OpenTyrian2000.
- **The 2-player assumption runs deep.** About 230 references to `player[1]`,
  `twoPlayerMode`, `thisPlayerNum` and similar across 11 files, mostly
  `mainint.c`, `tyrian2.c`, `varz.c` and `config.c`. `Player player[2]` itself
  is a clean struct (`player.h`), which helps.

## Architecture: server-sequenced lockstep

```
  PC A (P1, P2)          relay (droplet)            PC B (P3)      PC C (P4)
  ─────────────          ───────────────            ─────────      ─────────
  input(T+D) ──────────►  collect inputs for tick T  ◄──── input ◄──── input
                          deadline hit → bundle T
  simulate T  ◄────────── TickBundle{T, in[4]} ─────► simulate T ─► simulate T
  hash every N ticks ───► compare hashes ◄─────────── hash ◄────── hash
```

1. Each tick, every client samples its **local** players' input into a
   `PlayerInput { buttons, dx, dy }` for tick `T + D`, where `D` is the input
   delay (default 2–3 ticks) that hides network latency.
2. The **relay** collects the inputs for tick `T` from every slot and
   broadcasts one `TickBundle { T, input[4] }`. If a slot's input misses the
   deadline, the relay repeats that slot's previous input (flagged) instead of
   freezing everyone. A player who keeps missing deadlines gets dropped.
3. Clients simulate tick `T` only after `TickBundle T` arrives, so every PC
   applies exactly the same inputs in exactly the same order.
4. Every N ticks each client sends a **hash of the simulation state**
   (players, enemies, shots, RNG state, score, `curLoc`). The relay compares
   the hashes. On a mismatch, every client stops with "desync at tick T" and
   writes a state dump for debugging.

**Why a relay instead of peer-to-peer:**

- Every client makes one outbound connection to a known server. That works
  behind any home router or mobile carrier NAT, with no hole punching.
- Four players peer-to-peer would mean 6 links, and any one of them failing
  breaks the game.
- A single authority deciding each tick's inputs removes a whole class of
  "who's right?" bugs.
- The cost is one extra network hop. Pick a droplet region central to the
  group.

**One codebase, two ways to host:**

- The relay logic is a small, self-contained C module. It builds both as a
  standalone server (`tyrian-relay`) and inside the game ("Host on this PC").
- LAN and direct play therefore never need the droplet. The droplet is only
  for internet games with room codes.

**Transport:** ENet replaces SDL2_net. ENet is reliable and unreliable UDP
channels in a small C library, packaged for MSYS2 (`mingw-w64-ucrt-x86_64-enet`)
and Debian/Ubuntu (`libenet-dev`).

- Inputs and bundles go on an unreliable sequenced channel, with the last few
  ticks of input sent redundantly in each packet.
- Lobby, menu choices and hashes go on a reliable channel.

## Lobby and server

- **Rooms:** keyed by a 6-character code. The host creates a room with its
  settings (mode, episode, difficulty). Others join with the code, slots get
  assigned, and the host presses Start.
- **Version check:** a protocol version is checked on join.
- **The relay never runs game logic.** It only orders inputs and compares
  hashes, so it stays tiny and cheap.
- **Deployment:** the cheapest Ubuntu DigitalOcean droplet (a few dollars a
  month), a systemd unit, and the firewall opening one UDP port. A
  `tools/deploy-relay.sh` script builds and installs it. No accounts and no
  database.
- **Abuse limits:** per-IP rate limiting, room expiry, and caps on room count
  and packet size.

## 4-player core changes

- Define `MAX_PLAYERS 4` and make `Player player[MAX_PLAYERS]`. Replace
  `twoPlayerMode` with an active-slot count or mask. Generalize code that
  picks between exactly two players: enemy targeting, `all_players_dead()`,
  item pickup messages, linking.
- **Ships (duplicated for now):**
  - P1 and P3 fly the main ship.
  - P2 and P4 fly the Dragonwing (`is_dragonwing`).
  - P4 can link to P3 the same way P2 links to P1.
  - Copies are told apart by a palette tint and a player-number marker.
  - Per-slot ship choice comes later, together with new ship types.
- **HUD:** the 320×200 screen currently has side panels for two players. Four
  players need a compact layout, which needs its own design pass.
- **Local input:** two control sets per computer (keyboard layout A,
  keyboard layout B, mouse, gamepads), one for each of the computer's at
  most 2 players. Today both local players share one key layout, so the
  second needs its own.
- **Saves:** 2-player saves use their own slots (`twoPlayerMode ? 22 : 11`).
  Online games won't save at first.

## Cross-platform determinism (PC ↔ Mac)

Lockstep only works if every machine computes **bit-identical** results.

- **Integer maths** is the same everywhere.
- **Basic float operations** (`+ − × ÷`, `sqrtf`, `roundf`) are exactly
  specified by IEEE 754 and identical everywhere, as long as the compiler
  doesn't fuse `a*b+c` into one instruction. `-ffp-contract=off` is set in the
  Makefile for that; clang, used on macOS, fuses by default.
- **Library trig (`sinf`, `cosf`) is not guaranteed** to match between Windows,
  macOS and Linux. The simulation uses `dm_sinf`/`dm_cosf` (`src/dmath.c`)
  instead. These use only basic operations, and agree with libm to within
  1e-6 (same pixel after rounding). Visual-only code (starfield, menus, audio)
  keeps libm.
- **The RNG** (Mersenne Twister) uses `unsigned long`, which is 32 bits on
  Windows and 64 on macOS. Its values are masked to 32 bits, so it matches.
- **Evaluation order is not portable.** In `f(mt_rand(), mt_rand())` or
  `mt_rand() % a + mt_rand() % b`, C leaves the order unspecified, and gcc
  and clang pick opposite orders. This caused a real Windows/macOS desync,
  so random draws go into named locals first, one per statement. The same
  applies to any two side-effecting calls in one expression.
- **Settings that change gameplay** must match on every machine: difficulty,
  and possibly detail level or game speed if they alter what the simulation
  does. The host's settings will be sent at game start (M3).
- **CI** builds on Windows, Linux and macOS (Apple Silicon). It replays a
  recorded game on each and checks the state hash every tick
  (`tests/sync/`).

## Milestones

| # | Milestone | Done when |
|---|-----------|-----------|
| M0 | Infrastructure ✅ | Fork on GitHub, local MSYS2 build, CI building Windows, Linux and macOS. |
| M1 | Input layer + determinism tools ✅ | `JE_playerMovement()` reads input into a `PlayerInput` and applies it separately. Input recorder/replayer and a per-tick state hash exist (`src/sync.c`; `--record-input`, `--replay-input`, `--hash-log`, `--state-dump`). Replays verify on Windows, Linux and macOS in CI. Gameplay unchanged. Adversarially reviewed (3 rounds). |
| M2 | 4-player core | 4 player slots in the simulation, duplicated ships with tints, 4-player HUD in the widescreen side panels, a second control set so a computer can host its 2 players. No 4-on-one-computer mode: players 3–4 are verified with replays (their input comes from the recording) until M3 lets two computers play for real. |
| M3 | Netcode v2 on LAN | ENet transport, embedded relay, in-game Host/Join, 2–4 players, desync reporting. Old `network.c` removed. |
| M4 | Internet | Standalone relay on a droplet, room codes, deploy script, ping display, adaptive input delay. |
| M5 | Beyond | Story mode online (shop/menu sync), rejoin after disconnect, new ship types. |

M1 comes first because it is the foundation for everything else. The input
abstraction is what both the 4-player core and the new netcode build on, and
the replay hash check is how we prove the game stays in sync before involving
a network.

## Open questions

- Should online start as **arcade only**, adding story mode with its shop
  later? Recommended: yes.
- Shared or separate lives, and friendly fire, in 4-player games?
- Which droplet region suits the group?
- **Licensing.** The code is GPLv2, so the fork, including the relay, stays
  GPLv2. The Tyrian 2000 data files are freeware but not ours. Releases
  should either download them from the official link or check their
  redistribution terms first.
