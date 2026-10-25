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
#ifndef NETPLAY_H
#define NETPLAY_H

#include "netproto.h"
#include "opentyr.h"
#include "sync.h"

/*
 * Online play, the game's side: connecting to a relay (relay.c), the lobby, and
 * deterministic lockstep in game.  At the start of every simulation tick each
 * computer sends its local input for a few ticks ahead and then waits for the
 * relay's bundle of everyone's input for this tick, so all of them simulate the
 * same ticks from the same inputs.
 */

/** True while a network game is being played (from the start until it ends). */
extern bool isNetworkGame;
/** The local player's number (1 or 2) in a network game. */
extern uint thisPlayerNum;

/** The internet relay games use unless opentyrian.cfg or --relay names another (a DigitalOcean droplet, SFO2). */
#define NET_DEFAULT_RELAY "206.189.70.45"

/** The local player's name and the internet relay ("host" or "host:port"), from opentyrian.cfg. */
extern char net_player_name[NET_NAME_LEN];
extern char net_relay_address[128];
/** --net-name and --relay: used instead of the above for this run, without being saved. */
extern const char *net_name_option, *net_relay_option;

const char *net_current_name(void);
const char *net_current_relay(void);

typedef enum
{
	NET_OFFLINE,     // not connected; net_error() says why if a connection ended
	NET_CONNECTING,
	NET_IN_LOBBY,    // in a room, between games
	NET_IN_GAME,
} NetStatus;

typedef struct
{
	bool in_game;            // a game is running (or just ended and not everyone is back)
	unsigned int host_slot;
	unsigned int max_players;
	unsigned int my_slot, my_slot_count;
	unsigned int episode, difficulty, game_speed, detail;
	bool used[NET_MAX_SLOTS];
	char names[NET_MAX_SLOTS][NET_NAME_LEN];
} NetLobby;

NetStatus net_status(void);
const char *net_error(void);

/** Connects to a relay and creates a room (code NULL) or joins one. */
bool net_connect(const char *address, uint16_t default_port, const char *code);
/** Hosts on this computer: starts a relay here (LAN games) and creates a room on it. */
bool net_host_here(void);
/** Disconnects and stops this computer's relay. */
void net_leave(void);

/** Handles network traffic without waiting.  Called from service_SDL_events(). */
void net_poll(void);

const char *net_room_code(void);
const NetLobby *net_lobby(void);
bool net_is_host(void);
unsigned int net_lobby_players(void);
/** True while this computer is running the relay (hosting a LAN game). */
bool net_hosting_here(void);
/** This computer's LAN address, for showing to LAN players (empty if unknown). */
const char *net_local_address(void);

void net_send_settings(unsigned int episode, unsigned int difficulty);
void net_send_start(void);
/** True once the host started a game; net_begin_game() then sets it up. */
bool net_start_received(void);

/** Sets up the game the room started, like --quick-arcade does, on every computer. */
void net_begin_game(void);
/** Called when the game loop returns to the menus: reports back to the lobby. */
void net_end_game(void);
/** True once the game must end (quit, a desync, a player left); the level loop then exits. */
bool net_game_ended(void);
/** Why the last game ended, shown in the lobby (empty if it ended normally). */
const char *net_end_message(void);
void net_set_end_message(const char *message);

void net_tick_begin(void);
void net_tick_end(void);
/** The input `player_num` (1-based) uses this tick, from the relay's bundle. */
void net_get_input(unsigned int player_num, PlayerInput *input);

/** The name of player `player_num` (1-based) in the room. */
const char *net_player_name_of(unsigned int player_num);

/* LAN discovery */
typedef struct
{
	char address[64];
	uint16_t port;
	char code[NET_CODE_LEN + 1];
	char host_name[NET_NAME_LEN];
	unsigned int players, max_players;
	bool in_game;
} NetLanGame;

#define NET_MAX_LAN_GAMES 8

void net_discovery_start(void);
/** Sends discovery queries now and then and collects answers; returns how many games are known. */
unsigned int net_discovery_poll(NetLanGame *games, unsigned int max);
void net_discovery_stop(void);

/** Sets the defaults for net_player_name and net_relay_address, before the config file is read. */
void net_set_default_config(void);

#endif /* NETPLAY_H */
