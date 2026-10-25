/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2009  The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifndef SYNC_H
#define SYNC_H

#include "opentyr.h"

#include "SDL.h"

/*
 * Simulation sync: per-player input as plain data, a fingerprint (hash) of the
 * simulation state, and recording/replaying input.  Lockstep multiplayer only
 * works if every machine turns the same inputs into the same state; replays
 * and the per-tick hash are how that is checked.
 */

#define MAX_INPUT_PLAYERS 4

enum
{
	PLAYER_INPUT_FIRE           = 1 << 0,
	PLAYER_INPUT_LEFT_SIDEKICK  = 1 << 1,
	PLAYER_INPUT_RIGHT_SIDEKICK = 1 << 2,
	PLAYER_INPUT_CHANGE_FIRE    = 1 << 3,
};

/** One player's raw input for one simulation tick. */
typedef struct
{
	Sint16 move_x, move_y;      // digital movement in pixels (keyboard, d-pad, demo)
	Sint32 analog_x, analog_y;  // analog movement (mouse, analog stick), before clamping
	Uint8 buttons;              // PLAYER_INPUT_* bits
	bool link_gun_analog;       // analog stick is aiming the linked turret
	float link_gun_angle;
} PlayerInput;

/** Simulation ticks since the current level started. */
extern Uint32 sim_tick;

bool sync_open_record(const char *path);
bool sync_open_replay(const char *path);
bool sync_open_hash_log(const char *path);

/** Dump every hashed value at the end of `tick`, or (tick 0) when a replay diverges. */
void sync_set_state_dump(const char *path, Uint32 tick);

/** True when the simulation must be reproducible (recording or replaying). */
bool sync_is_deterministic(void);
bool sync_is_replaying(void);

void sync_level_start(unsigned int level);
void sync_tick_begin(void);
void sync_tick_end(void);

void sync_record_input(unsigned int player_num, const PlayerInput *input);
void sync_replay_input(unsigned int player_num, PlayerInput *input);

Uint64 sync_state_hash(void);

#endif /* SYNC_H */
