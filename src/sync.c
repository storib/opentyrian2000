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
#include "sync.h"

#include "backgrnd.h"
#include "config.h"
#include "mtrand.h"
#include "player.h"
#include "shots.h"
#include "varz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Replay file: "OTRP", u16 version, then a stream of records, all little-endian:
 *   'L' u16 level                       a level started; the tick count restarts
 *   'I' u8 player u32 tick <input>      input consumed by a player
 *   'G' u32 tick u8 event u8 value      a game event outside PlayerInput (SYNC_EVENT_*)
 *   'H' u32 tick u64 hash               state hash at the end of a tick
 *   'E' u32 hashes                      end of recording; number of 'H' records
 * Records appear in the order the simulation produced them, so a replay reads
 * them back in that same order and any mismatch means the simulation diverged.
 * A file without the 'E' record is incomplete (the recording crashed or was
 * killed) and never counts as a successful replay.
 */
#define REPLAY_MAGIC   "OTRP"
#define REPLAY_VERSION 2

// exit codes of a replay (the dev script timeout uses 4)
#define REPLAY_EXIT_DIVERGED  3
#define REPLAY_EXIT_TRUNCATED 5

Uint32 sim_tick = 0;

static FILE *record_file = NULL, *replay_file = NULL, *hash_log = NULL;

static unsigned int current_level = 0;
static Uint32 recorded_hashes = 0, verified_ticks = 0;

static const char *state_dump_path = NULL;
static Uint32 state_dump_tick = 0;  // 0: only when a replay diverges
static bool state_dumped = false;

// While dumping, every hashed value is also written out by name, so dumps from two
// machines can be diffed to find exactly what diverged.
static FILE *dump_file = NULL;
static char dump_context[32] = "";  // which entity the values belong to, e.g. "enemy[12]."

static void write_state_dump(void);

/* --- byte packing --- */

static void put_u8(FILE *f, Uint8 v)
{
	fputc(v, f);
}

static void put_u16(FILE *f, Uint16 v)
{
	put_u8(f, v & 0xff);
	put_u8(f, v >> 8);
}

static void put_u32(FILE *f, Uint32 v)
{
	put_u16(f, v & 0xffff);
	put_u16(f, v >> 16);
}

static void put_u64(FILE *f, Uint64 v)
{
	put_u32(f, v & 0xffffffff);
	put_u32(f, v >> 32);
}

static bool get_u8(FILE *f, Uint8 *v)
{
	int c = fgetc(f);
	*v = c == EOF ? 0 : c;
	return c != EOF;
}

static bool get_u16(FILE *f, Uint16 *v)
{
	Uint8 lo = 0, hi = 0;
	bool ok = get_u8(f, &lo) && get_u8(f, &hi);
	*v = lo | (hi << 8);
	return ok;
}

static bool get_u32(FILE *f, Uint32 *v)
{
	Uint16 lo = 0, hi = 0;
	bool ok = get_u16(f, &lo) && get_u16(f, &hi);
	*v = lo | ((Uint32)hi << 16);
	return ok;
}

static bool get_u64(FILE *f, Uint64 *v)
{
	Uint32 lo = 0, hi = 0;
	bool ok = get_u32(f, &lo) && get_u32(f, &hi);
	*v = lo | ((Uint64)hi << 32);
	return ok;
}

static Uint32 float_bits(float f)
{
	Uint32 bits;
	memcpy(&bits, &f, sizeof(bits));
	return bits;
}

static float bits_float(Uint32 bits)
{
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

/* --- opening --- */

/** Runs at exit: marks the recording as complete. A crashed recording has no end record. */
static void finish_recording(void)
{
	if (record_file == NULL)
		return;

	put_u8(record_file, 'E');
	put_u32(record_file, recorded_hashes);

	const bool write_failed = ferror(record_file) != 0;
	if (fclose(record_file) != 0 || write_failed)
		fprintf(stderr, "error: failed to write the replay file\n");
	record_file = NULL;
}

bool sync_open_record(const char *path)
{
	if (replay_file != NULL)
	{
		fprintf(stderr, "error: cannot record and replay at the same time\n");
		return false;
	}

	record_file = fopen(path, "wb");
	if (record_file == NULL)
	{
		fprintf(stderr, "error: failed to create replay file '%s'\n", path);
		return false;
	}

	fwrite(REPLAY_MAGIC, 1, 4, record_file);
	put_u16(record_file, REPLAY_VERSION);
	atexit(finish_recording);
	return true;
}

bool sync_open_replay(const char *path)
{
	if (record_file != NULL)
	{
		fprintf(stderr, "error: cannot record and replay at the same time\n");
		return false;
	}

	replay_file = fopen(path, "rb");
	if (replay_file == NULL)
	{
		fprintf(stderr, "error: failed to open replay file '%s'\n", path);
		return false;
	}

	char magic[4];
	Uint16 version;
	if (fread(magic, 1, 4, replay_file) != 4 || memcmp(magic, REPLAY_MAGIC, 4) != 0 ||
	    !get_u16(replay_file, &version) || version != REPLAY_VERSION)
	{
		fprintf(stderr, "error: '%s' is not a version %d replay file\n", path, REPLAY_VERSION);
		return false;
	}
	return true;
}

bool sync_open_hash_log(const char *path)
{
	hash_log = fopen(path, "w");
	if (hash_log == NULL)
	{
		fprintf(stderr, "error: failed to create hash log '%s'\n", path);
		return false;
	}
	return true;
}

void sync_set_state_dump(const char *path, Uint32 tick)
{
	state_dump_path = path;
	state_dump_tick = tick;
}

bool sync_is_deterministic(void)
{
	return record_file != NULL || replay_file != NULL;
}

bool sync_is_replaying(void)
{
	return replay_file != NULL;
}

/* --- replay checking --- */

static void replay_exit(int code, const char *log_line)
{
	if (hash_log != NULL)
	{
		if (log_line != NULL)
			fprintf(hash_log, "%s\n", log_line);
		fclose(hash_log);
	}
	exit(code);
}

static void replay_truncated(void)
{
	fprintf(stderr, "replay: FAILED, the replay file is truncated or corrupt at byte %ld; %u ticks verified before\n",
	        ftell(replay_file), (unsigned)verified_ticks);
	replay_exit(REPLAY_EXIT_TRUNCATED, "TRUNCATED");
}

static void replay_diverged(const char *what, Uint64 expected, Uint64 actual)
{
	fprintf(stderr, "replay: DIVERGED at level %u tick %u: %s (expected %llx, got %llx); %u ticks verified before\n",
	        current_level, (unsigned)sim_tick, what,
	        (unsigned long long)expected, (unsigned long long)actual, (unsigned)verified_ticks);
	if (state_dump_path != NULL && state_dump_tick == 0)
		write_state_dump();
	replay_exit(REPLAY_EXIT_DIVERGED, "DIVERGED");
}

/** The end record was reached: every recorded tick must have been verified. */
static void replay_finished(void)
{
	Uint32 hashes = 0;
	if (!get_u32(replay_file, &hashes) || fgetc(replay_file) != EOF)
		replay_truncated();
	if (hashes != verified_ticks)
		replay_diverged("number of ticks", hashes, verified_ticks);

	fprintf(stderr, "replay: finished, %u ticks verified\n", (unsigned)verified_ticks);
	replay_exit(0, NULL);
}

/* reads that end the replay as truncated when the file runs out */
static Uint8 replay_u8(void)   { Uint8 v;  if (!get_u8(replay_file, &v))  replay_truncated(); return v; }
static Uint16 replay_u16(void) { Uint16 v; if (!get_u16(replay_file, &v)) replay_truncated(); return v; }
static Uint32 replay_u32(void) { Uint32 v; if (!get_u32(replay_file, &v)) replay_truncated(); return v; }
static Uint64 replay_u64(void) { Uint64 v; if (!get_u64(replay_file, &v)) replay_truncated(); return v; }

/** Writes every value of the state hash, by name, for the current tick. */
static void write_state_dump(void)
{
	dump_file = fopen(state_dump_path, "w");
	if (dump_file == NULL)
	{
		fprintf(stderr, "error: failed to create state dump '%s'\n", state_dump_path);
		return;
	}

	fprintf(dump_file, "level %u tick %u\n", current_level, (unsigned)sim_tick);
	const Uint64 dumped_hash = sync_state_hash();
	fprintf(dump_file, "hash %016llx\n", (unsigned long long)dumped_hash);

	fclose(dump_file);
	dump_file = NULL;
	fprintf(stderr, "sync: wrote state dump for tick %u to %s\n", (unsigned)sim_tick, state_dump_path);
}

/** Reads the next record type; the end record finishes the replay. */
static void replay_next(Uint8 expected_type)
{
	const Uint8 type = replay_u8();
	if (type == 'E')
		replay_finished();
	if (type != expected_type)
		replay_diverged("record order", expected_type, type);
}

void sync_replay_check_end(void)
{
	if (replay_file == NULL)
		return;

	const int type = fgetc(replay_file);
	if (type == 'E')
		replay_finished();
	if (type != EOF)
		ungetc(type, replay_file);
}

/* --- level and tick --- */

void sync_level_start(unsigned int level)
{
	current_level = level;
	sim_tick = 0;

	if (record_file != NULL)
	{
		put_u8(record_file, 'L');
		put_u16(record_file, level);
	}

	if (replay_file != NULL)
	{
		replay_next('L');
		const Uint16 recorded_level = replay_u16();
		if (recorded_level != level)
			replay_diverged("level", recorded_level, level);
	}

	if (hash_log != NULL)
		fprintf(hash_log, "L %u\n", level);

	// Fixed seed so a level always plays out the same way from the same inputs.
	if (sync_is_deterministic())
		mt_srand(32402394);
}

void sync_tick_begin(void)
{
	++sim_tick;
}

void sync_tick_end(void)
{
	if (record_file == NULL && replay_file == NULL && hash_log == NULL && state_dump_path == NULL)
		return;

	const Uint64 hash = sync_state_hash();

	// only the first level that reaches the tick, so the dump isn't overwritten each level
	if (state_dump_path != NULL && state_dump_tick == sim_tick && !state_dumped)
	{
		write_state_dump();
		state_dumped = true;
	}

	if (record_file != NULL)
	{
		put_u8(record_file, 'H');
		put_u32(record_file, sim_tick);
		put_u64(record_file, hash);
		++recorded_hashes;
	}

	if (replay_file != NULL)
	{
		replay_next('H');
		const Uint32 tick = replay_u32();
		const Uint64 recorded_hash = replay_u64();
		if (tick != sim_tick)
			replay_diverged("tick", tick, sim_tick);
		if (recorded_hash != hash)
			replay_diverged("state hash", recorded_hash, hash);
		++verified_ticks;
	}

	if (hash_log != NULL)
		fprintf(hash_log, "%u %016llx\n", (unsigned)sim_tick, (unsigned long long)hash);
}

/* --- input --- */

void sync_record_input(unsigned int player_num, const PlayerInput *input)
{
	if (record_file == NULL)
		return;

	put_u8(record_file, 'I');
	put_u8(record_file, player_num);
	put_u32(record_file, sim_tick);
	put_u16(record_file, input->move_x);
	put_u16(record_file, input->move_y);
	put_u32(record_file, input->analog_x);
	put_u32(record_file, input->analog_y);
	put_u8(record_file, input->buttons);
	put_u8(record_file, input->link_gun_analog);
	put_u32(record_file, float_bits(input->link_gun_angle));
}

void sync_replay_input(unsigned int player_num, PlayerInput *input)
{
	replay_next('I');

	// one read per statement: the order of reads must be fixed
	const Uint8 recorded_player = replay_u8();
	const Uint32 tick = replay_u32();
	const Uint16 move_x = replay_u16();
	const Uint16 move_y = replay_u16();
	const Uint32 analog_x = replay_u32();
	const Uint32 analog_y = replay_u32();
	const Uint8 buttons = replay_u8();
	const Uint8 link_gun_analog = replay_u8();
	const Uint32 angle = replay_u32();

	if (recorded_player != player_num)
		replay_diverged("input player", recorded_player, player_num);
	if (tick != sim_tick)
		replay_diverged("input tick", tick, sim_tick);

	input->move_x = (Sint16)move_x;
	input->move_y = (Sint16)move_y;
	input->analog_x = (Sint32)analog_x;
	input->analog_y = (Sint32)analog_y;
	input->buttons = buttons;
	input->link_gun_analog = link_gun_analog;
	input->link_gun_angle = bits_float(angle);
}

/* --- game events --- */

bool sync_event(Uint8 event, bool local_value)
{
	if (record_file != NULL)
	{
		put_u8(record_file, 'G');
		put_u32(record_file, sim_tick);
		put_u8(record_file, event);
		put_u8(record_file, local_value);
		return local_value;
	}

	if (replay_file != NULL)
	{
		replay_next('G');
		const Uint32 tick = replay_u32();
		const Uint8 recorded_event = replay_u8();
		const Uint8 value = replay_u8();
		if (tick != sim_tick)
			replay_diverged("event tick", tick, sim_tick);
		if (recorded_event != event)
			replay_diverged("event", recorded_event, event);
		return value != 0;
	}

	return local_value;
}

/* --- state hash --- */

// FNV-1a over values, never raw memory: structs hold pointers and padding that
// differ between runs and between platforms.
static Uint64 hash;

static void hash_value(const char *name, int index, Sint64 value)
{
	if (dump_file != NULL)
	{
		if (index >= 0)
			fprintf(dump_file, "%s%s[%d]=%lld\n", dump_context, name, index, (long long)value);
		else
			fprintf(dump_file, "%s%s=%lld\n", dump_context, name, (long long)value);
	}

	Uint64 v = (Uint64)value;
	for (int i = 0; i < 8; ++i)
	{
		hash ^= v & 0xff;
		hash *= 1099511628211ULL;
		v >>= 8;
	}
}

#define H(v) hash_value(#v, -1, (Sint64)(v))
#define H_FLOAT(v) hash_value(#v, -1, float_bits(v))
#define H_ARRAY(a) do { for (size_t h_i = 0; h_i < COUNTOF(a); ++h_i) hash_value(#a, (int)h_i, (Sint64)(a)[h_i]); } while (false)
#define CONTEXT(...) do { if (dump_file != NULL) snprintf(dump_context, sizeof(dump_context), __VA_ARGS__); } while (false)

static void hash_player(const Player *p)
{
	H(p->cash);

	const PlayerItems *items = &p->items;
	H(items->ship);
	H(items->generator);
	H(items->shield);
	for (uint i = 0; i < COUNTOF(items->weapon); ++i)
	{
		H(items->weapon[i].id);
		H(items->weapon[i].power);
	}
	H_ARRAY(items->sidekick);
	H(items->special);
	H(items->sidekick_series);
	H(items->sidekick_level);
	H(items->super_arcade_mode);

	H(p->is_dragonwing);
	H(p->lives != NULL ? *p->lives : 0);
	H(p->is_alive);
	H(p->invulnerable_ticks);
	H(p->exploding_ticks);
	H(p->shield);
	H(p->armor);
	H(p->weapon_mode);
	H(p->superbombs);
	H(p->purple_balls_needed);
	H(p->x);
	H(p->y);
	H_ARRAY(p->old_x);
	H_ARRAY(p->old_y);
	H(p->x_velocity);
	H(p->y_velocity);
	H(p->x_friction_ticks);
	H(p->y_friction_ticks);
	H(p->delta_x_shot_move);
	H(p->delta_y_shot_move);
	H(p->last_x_shot_move);
	H(p->last_y_shot_move);
	H(p->last_x_explosion_follow);
	H(p->last_y_explosion_follow);

	for (uint i = 0; i < COUNTOF(p->sidekick); ++i)
	{
		H(p->sidekick[i].x);
		H(p->sidekick[i].y);
		H(p->sidekick[i].ammo);
		H(p->sidekick[i].ammo_refill_ticks);
		H(p->sidekick[i].animation_enabled);
		H(p->sidekick[i].animation_frame);
		H(p->sidekick[i].charge);
		H(p->sidekick[i].charge_ticks);
	}
}

static void hash_enemy(const struct JE_SingleEnemyType *e)
{
	H(e->ex); H(e->ey);
	H(e->exc); H(e->eyc);
	H(e->exca); H(e->eyca);
	H(e->excc); H(e->eycc);
	H(e->exccw); H(e->eyccw);
	H(e->armorleft);
	H_ARRAY(e->eshotwait);
	H_ARRAY(e->eshotmultipos);
	H(e->enemycycle);
	H(e->ani);
	H_ARRAY(e->egr);
	H(e->size);
	H(e->linknum);
	H(e->aniactive);
	H(e->animax);
	H(e->aniwhenfire);
	H(e->exrev); H(e->eyrev);
	H(e->exccadd); H(e->eyccadd);
	H(e->exccwmax); H(e->eyccwmax);
	H(e->edamaged);
	H(e->enemytype);
	H(e->animin);
	H(e->edgr);
	H(e->edlevel);
	H(e->edani);
	H(e->filter);
	H(e->evalue);
	H(e->fixedmovey);
	H_ARRAY(e->freq);
	H(e->launchwait);
	H(e->launchtype);
	H(e->launchfreq);
	H(e->xaccel); H(e->yaccel);
	H_ARRAY(e->tur);
	H(e->enemydie);
	H(e->enemyground);
	H(e->explonum);
	H(e->mapoffset);
	H(e->scoreitem);
	H(e->special);
	H(e->flagnum);
	H(e->setto);
	H(e->iced);
	H(e->launchspecial);
	H(e->xminbounce); H(e->xmaxbounce);
	H(e->yminbounce); H(e->ymaxbounce);
}

static void hash_player_shot(const PlayerShotDataType *s)
{
	H(s->shotX); H(s->shotY);
	H(s->shotXM); H(s->shotYM);
	H(s->shotXC); H(s->shotYC);
	H(s->shotComplicated);
	H(s->shotDevX); H(s->shotDirX);
	H(s->shotDevY); H(s->shotDirY);
	H(s->shotCirSizeX); H(s->shotCirSizeY);
	H(s->shotTrail);
	H(s->shotGr); H(s->shotAni); H(s->shotAniMax);
	H(s->shotDmg);
	H(s->shotBlastFilter);
	H(s->chainReaction);
	H(s->playerNumber);
	H(s->aimAtEnemy);
	H(s->aimDelay); H(s->aimDelayMax);
}

static void hash_enemy_shot(const EnemyShotType *s)
{
	H(s->sx); H(s->sy);
	H(s->sxm); H(s->sym);
	H(s->sxc); H(s->syc);
	H(s->tx); H(s->ty);
	H(s->sgr);
	H(s->sdmg);
	H(s->duration);
	H(s->animate);
	H(s->animax);
}

Uint64 sync_state_hash(void)
{
	hash = 14695981039346656037ULL;

	CONTEXT("%s", "");
	H(mt_state_digest());

	for (uint i = 0; i < COUNTOF(player); ++i)
	{
		CONTEXT("player[%u].", i);
		hash_player(&player[i]);
	}

	for (uint i = 0; i < COUNTOF(enemy); ++i)
	{
		CONTEXT("enemy[%u].", i);
		H(enemyAvail[i]);
		if (enemyAvail[i] != 1)  // 1 means the slot is free
			hash_enemy(&enemy[i]);
	}

	for (uint i = 0; i < MAX_PWEAPON; ++i)
	{
		CONTEXT("player_shot[%u].", i);
		H(shotAvail[i]);
		if (shotAvail[i] != 0)  // 0 means the slot is free
			hash_player_shot(&playerShotData[i]);
	}

	for (uint i = 0; i < ENEMY_SHOT_MAX; ++i)
	{
		CONTEXT("enemy_shot[%u].", i);
		H(enemyShotAvail[i]);
		if (!enemyShotAvail[i])  // true means the slot is free
			hash_enemy_shot(&enemyShot[i]);
	}

	for (uint i = 0; i < MAX_EXPLOSIONS; ++i)
	{
		CONTEXT("explosion[%u].", i);
		H(explosions[i].ttl);
		if (explosions[i].ttl != 0)
		{
			H(explosions[i].x);
			H(explosions[i].y);
			H(explosions[i].sprite);
			H(explosions[i].followPlayer);
			H(explosions[i].fixedPosition);
			H(explosions[i].deltaY);
		}
	}

	// level progress and assorted gameplay globals
	CONTEXT("%s", "");
	H(curLoc);
	H(eventLoc);
	H(maxEvent);
	H(levelEnd);
	H(levelEndFxWait);
	H(levelEndWarp);
	H(endLevel);
	H(reallyEndLevel);
	H(totalEnemy);
	H(enemyKilled);
	H(enemyOnScreen);
	H(levelTimer);
	H(levelTimerCountdown);
	H(difficultyLevel);
	H_ARRAY(globalFlags);
	H(twoPlayerLinked);
	H_FLOAT(linkGunDirec);
	H_ARRAY(shotRepeat);
	H_ARRAY(shotMultiPos);
	H(chargeWait);
	H(chargeLevel);
	H(chargeMax);
	H_FLOAT(optionSatelliteRotate);
	H(optionAttachmentMove);
	H(zinglonDuration);
	H(astralDuration);
	H(flareDuration);
	H(specialWait);
	H(nextSpecialWait);
	H(nextLevel);
	H(playerEndLevel);
	H(readyToEndLevel);
	H(enemiesActive);
	H(stopBackgrounds);
	H(stopBackgroundNum);
	H(power);
	H(shieldWait);
	H(superArcadePowerUp);
	H(cubeMax);
	H_ARRAY(smoothies);
	for (uint i = 0; i < COUNTOF(SFCurrentCode); ++i)
		H_ARRAY(SFCurrentCode[i]);
	H_ARRAY(SFExecuted);

	// scrolling: positions as offsets into the maps, never as addresses
	H(backPos); H(backPos2); H(backPos3);
	H(backMove); H(backMove2); H(backMove3);
	H(mapYPos  != NULL ? mapYPos  - &megaData1.mainmap[0][0] : -1);
	H(mapY2Pos != NULL ? mapY2Pos - &megaData2.mainmap[0][0] : -1);
	H(mapY3Pos != NULL ? mapY3Pos - &megaData3.mainmap[0][0] : -1);

	for (uint i = 0; i < MAX_REPEATING_EXPLOSIONS; ++i)
	{
		CONTEXT("rep_explosion[%u].", i);
		H(rep_explosions[i].ttl);
		if (rep_explosions[i].ttl != 0)
		{
			H(rep_explosions[i].delay);
			H(rep_explosions[i].x);
			H(rep_explosions[i].y);
			H(rep_explosions[i].big);
		}
	}

	return hash;
}
