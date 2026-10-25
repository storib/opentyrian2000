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
#ifndef NETPROTO_H
#define NETPROTO_H

/*
 * The network protocol shared by the game (netplay.c) and the relay (relay.c).
 * Plain C with no SDL, so the standalone relay builds on a bare server.
 *
 * Every computer connects to one relay over ENet.  The relay orders the inputs:
 * once it has every player's input for tick T it sends all of them out as the
 * bundle for tick T, and each computer simulates T only from that bundle.  It
 * never runs game logic; it also compares the state hashes the computers send.
 *
 * All integers are little-endian.  Strings are fixed-size and NUL-padded.
 */

#define NET_MAGIC            0x4E525954u  // "TYRN"
#define NET_PROTOCOL_VERSION 1            // bump whenever the protocol or the simulation changes incompatibly

#define NET_DEFAULT_PORT     21900        // UDP, the relay
#define NET_DISCOVERY_PORT   21901        // UDP, LAN game discovery broadcasts

#define NET_MAX_SLOTS        4            // player slots in a room
#define NET_MAX_LOCAL        2            // players on one computer, ever
#define NET_CODE_LEN         6            // room code characters
#define NET_NAME_LEN         16           // player name bytes on the wire (15 characters + NUL)
#define NET_INPUT_SIZE       18           // one serialized PlayerInput
#define NET_TICKS_PER_PACKET 16           // most ticks of input or bundles in one packet

// the game settings a room can have (the game's episode, difficulty, game speed and detail level)
#define NET_MAX_EPISODE      5
#define NET_MIN_DIFFICULTY   1            // Easy
#define NET_MAX_DIFFICULTY   3            // Hard
#define NET_MAX_GAME_SPEED   5
#define NET_MAX_DETAIL       6

// characters used in room codes: no 0/O, 1/I/L, so a code read out loud can't be misheard
#define NET_CODE_ALPHABET "ABCDEFGHJKMNPQRSTUVWXYZ23456789"

enum
{
	NET_CHANNEL_CONTROL = 0,  // reliable: lobby, start, pauses, game end
	NET_CHANNEL_TICKS   = 1,  // unreliable: inputs and bundles, each packet repeats what isn't acknowledged yet
	NET_CHANNELS
};

/* Client to relay */
enum
{
	NET_C_HELLO    = 0x01,  // u32 magic, u16 version, u8 action (NET_HELLO_*), u8 local players,
	                        // u8 max players (create), char code[6] (join), char name[16]
	NET_C_SETTINGS = 0x02,  // host only: u8 episode, u8 difficulty, u8 game speed, u8 detail level
	NET_C_START    = 0x03,  // host only
	NET_C_INPUT    = 0x04,  // u32 bundle ack, u32 first tick, u8 count, then count entries of
	                        // { u32 hash tick, u64 hash, input[local players] }
	NET_C_RESUME   = 0x05,  // u32 hold tick, u8 quit: end a pause or the in-game menu
	NET_C_DONE     = 0x06,  // this computer has left the game and is back in the lobby
};

enum
{
	NET_HELLO_CREATE = 0,
	NET_HELLO_JOIN   = 1,
};

/* Relay to client */
enum
{
	NET_R_WELCOME = 0x81,  // char code[6]
	NET_R_REJECT  = 0x82,  // u8 reason (NET_REJECT_*)
	NET_R_LOBBY   = 0x83,  // u8 in game, u8 host slot, u8 max players, u8 your first slot, u8 your slot count,
	                       // u8 episode, u8 difficulty, u8 game speed, u8 detail level,
	                       // then for each slot { u8 used, char name[16] }
	NET_R_START   = 0x84,  // u8 players, u8 input delay, u8 episode, u8 difficulty, u8 game speed, u8 detail level
	NET_R_BUNDLE  = 0x85,  // u32 input ack, u32 first tick, u8 count, u8 players, then count × players inputs
	NET_R_RESUME  = 0x86,  // u32 hold tick, u8 quit, u8 slot that resumed
	NET_R_END     = 0x87,  // u8 reason (NET_END_*), u8 slot, u32 tick, u32 stop tick
};

enum
{
	NET_REJECT_VERSION     = 1,
	NET_REJECT_NO_ROOM     = 2,
	NET_REJECT_ROOM_FULL   = 3,
	NET_REJECT_IN_GAME     = 4,
	NET_REJECT_SERVER_FULL = 5,
	NET_REJECT_BAD_REQUEST = 6,
};

/*
 * Why the relay stopped a game.  It stops sending bundles after "stop tick", and every
 * computer simulates up to exactly that tick before ending, so they all end in the same
 * state (and their desync dumps can be compared line by line).
 */
enum
{
	NET_END_DESYNC      = 1,  // the state hashes for "tick" differed
	NET_END_PLAYER_LEFT = 2,  // "slot" disconnected
};

/*
 * LAN discovery (plain UDP to NET_DISCOVERY_PORT):
 *   query  "TYR?" u16 version
 *   answer "TYR!" u16 version, u16 relay port, char code[6], char host name[16],
 *          u8 players, u8 max players, u8 in game
 */
#define NET_DISCOVERY_QUERY_SIZE  6
#define NET_DISCOVERY_ANSWER_SIZE (4 + 2 + 2 + NET_CODE_LEN + NET_NAME_LEN + 3)

#endif /* NETPROTO_H */
