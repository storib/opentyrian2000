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
#include "relay.h"

#include "netproto.h"

#include <enet/enet.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RELAY_MAX_PEERS      256
#define RELAY_MAX_ROOMS      128
#define RELAY_MAX_PER_IP     8      // connections from one address
#define RELAY_MAX_PACKET     2048   // anything bigger isn't from the game
#define RELAY_MAX_RATE       400    // packets per second from one connection
#define RELAY_HELLO_TIMEOUT  5000   // ms a connection may stay without joining a room
#define RELAY_ROOM_LIFETIME  (12u * 60 * 60 * 1000)  // ms before a room closes, so rooms can't be hoarded
#define RELAY_TICK_RING      256    // ticks of input kept per room; far more than are ever in flight
#define RELAY_TICK_MS        29     // one game tick at normal speed, for picking the input delay
#define RELAY_MIN_DELAY      2
#define RELAY_MAX_DELAY      10

const char *relay_log_prefix = "relay: ";

typedef struct Room Room;

typedef struct
{
	ENetPeer *peer;
	Room *room;
	unsigned int first_slot, slot_count;
	char name[NET_NAME_LEN];
	enet_uint32 connected_at;

	uint32_t input_contig;  // every input up to this tick has arrived
	uint32_t bundle_ack;    // the client has every bundle up to this tick
	bool done;              // back in the lobby after a game
	bool rejected;          // refused; ignored until it disconnects

	enet_uint32 rate_start;
	unsigned int rate_count;
} Client;

typedef struct
{
	uint32_t tick;  // the tick the hash is for; 0 before the first one
	uint64_t hash;
} TickHash;

struct Room
{
	bool used;
	enet_uint32 created_at;
	char code[NET_CODE_LEN];
	Client *slots[NET_MAX_SLOTS];  // always filled from slot 0 up; a client with 2 players has 2
	unsigned int max_players;
	unsigned int host_slot;
	uint8_t episode, difficulty, game_speed, detail;

	bool in_game;
	bool stopped;                  // no more bundles: a desync or a player left
	unsigned int players;
	uint32_t next_tick;            // bundles exist for every tick before this
	uint32_t last_resume_tick;
	uint8_t inputs[RELAY_TICK_RING][NET_MAX_SLOTS][NET_INPUT_SIZE];
	TickHash hashes[RELAY_TICK_RING][NET_MAX_SLOTS];  // indexed by the sender's first slot
};

struct Relay
{
	ENetHost *host;
	ENetSocket discovery;
	uint16_t port;
	uint64_t rng;
	enet_uint32 last_check;
	Room rooms[RELAY_MAX_ROOMS];
};

/* --- helpers --- */

static void relay_log(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	fputs(relay_log_prefix, stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

static const char *peer_ip(const ENetPeer *peer)
{
	static char buffer[64];
	if (enet_address_get_host_ip(&peer->address, buffer, sizeof(buffer)) != 0)
		strcpy(buffer, "?");
	return buffer;
}

typedef struct
{
	const uint8_t *p, *end;
	bool ok;
} Reader;

static uint8_t get_u8(Reader *r)
{
	if (r->p + 1 > r->end)
	{
		r->ok = false;
		return 0;
	}
	return *r->p++;
}

static uint32_t get_u32(Reader *r)
{
	uint32_t v = get_u8(r);
	v |= (uint32_t)get_u8(r) << 8;
	v |= (uint32_t)get_u8(r) << 16;
	v |= (uint32_t)get_u8(r) << 24;
	return v;
}

static uint64_t get_u64(Reader *r)
{
	uint64_t v = get_u32(r);
	v |= (uint64_t)get_u32(r) << 32;
	return v;
}

static uint16_t get_u16(Reader *r)
{
	uint16_t v = get_u8(r);
	v |= (uint16_t)(get_u8(r) << 8);
	return v;
}

static const uint8_t *get_bytes(Reader *r, size_t n)
{
	if ((size_t)(r->end - r->p) < n)
	{
		r->ok = false;
		return NULL;
	}
	const uint8_t *bytes = r->p;
	r->p += n;
	return bytes;
}

static uint8_t *put_u8(uint8_t *p, uint8_t v)   { *p++ = v; return p; }
static uint8_t *put_u16(uint8_t *p, uint16_t v) { p = put_u8(p, v & 0xff); return put_u8(p, v >> 8); }
static uint8_t *put_u32(uint8_t *p, uint32_t v) { p = put_u16(p, v & 0xffff); return put_u16(p, v >> 16); }
static uint8_t *put_bytes(uint8_t *p, const void *bytes, size_t n) { memcpy(p, bytes, n); return p + n; }

static void send_data(ENetPeer *peer, int channel, const uint8_t *data, size_t length)
{
	const enet_uint32 flags = channel == NET_CHANNEL_CONTROL ? ENET_PACKET_FLAG_RELIABLE : 0;
	ENetPacket *packet = enet_packet_create(data, length, flags);
	if (packet != NULL && enet_peer_send(peer, (enet_uint8)channel, packet) != 0)
		enet_packet_destroy(packet);
}

static uint64_t next_random(Relay *relay)
{
	// xorshift64; room codes only need to be unpredictable enough not to collide
	relay->rng ^= relay->rng << 13;
	relay->rng ^= relay->rng >> 7;
	relay->rng ^= relay->rng << 17;
	return relay->rng;
}

/* --- rooms --- */

static unsigned int room_used_slots(const Room *room)
{
	unsigned int n = 0;
	while (n < NET_MAX_SLOTS && room->slots[n] != NULL)
		++n;
	return n;
}

/** Calls f once for each client in the room (a client can hold two slots). */
#define FOR_EACH_CLIENT(room, c) \
	for (unsigned int c##_slot = 0; c##_slot < NET_MAX_SLOTS; ++c##_slot) \
		for (Client *c = (room)->slots[c##_slot]; c != NULL && c->first_slot == c##_slot; c = NULL)

static void broadcast(Room *room, int channel, const uint8_t *data, size_t length)
{
	FOR_EACH_CLIENT(room, c)
		send_data(c->peer, channel, data, length);
}

static void send_lobby(Room *room)
{
	FOR_EACH_CLIENT(room, c)
	{
		uint8_t buffer[16 + NET_MAX_SLOTS * (1 + NET_NAME_LEN)];
		uint8_t *p = buffer;
		p = put_u8(p, NET_R_LOBBY);
		p = put_u8(p, room->in_game);
		p = put_u8(p, room->host_slot);
		p = put_u8(p, room->max_players);
		p = put_u8(p, c->first_slot);
		p = put_u8(p, c->slot_count);
		p = put_u8(p, room->episode);
		p = put_u8(p, room->difficulty);
		p = put_u8(p, room->game_speed);
		p = put_u8(p, room->detail);
		for (unsigned int i = 0; i < NET_MAX_SLOTS; ++i)
		{
			static const char no_name[NET_NAME_LEN];
			const Client *owner = room->slots[i];
			p = put_u8(p, owner != NULL);
			p = put_bytes(p, owner != NULL ? owner->name : no_name, NET_NAME_LEN);
		}
		send_data(c->peer, NET_CHANNEL_CONTROL, buffer, p - buffer);
	}
}

static void send_reject(ENetPeer *peer, uint8_t reason)
{
	const uint8_t data[2] = { NET_R_REJECT, reason };
	send_data(peer, NET_CHANNEL_CONTROL, data, sizeof(data));
	enet_peer_disconnect_later(peer, 0);

	// no second guess at a room code on the same connection
	Client *c = peer->data;
	if (c != NULL)
		c->rejected = true;
}

static void send_end(Room *room, uint8_t reason, unsigned int slot, uint32_t tick)
{
	uint8_t data[11];
	uint8_t *p = data;
	p = put_u8(p, NET_R_END);
	p = put_u8(p, reason);
	p = put_u8(p, slot);
	p = put_u32(p, tick);
	p = put_u32(p, room->next_tick - 1);
	broadcast(room, NET_CHANNEL_CONTROL, data, p - data);
}

/** Sends a client the bundles it hasn't acknowledged yet, and which of its inputs arrived. */
static void send_bundles(Room *room, Client *c)
{
	const uint32_t first = c->bundle_ack + 1;
	uint32_t count = first < room->next_tick ? room->next_tick - first : 0;
	if (count > NET_TICKS_PER_PACKET)
		count = NET_TICKS_PER_PACKET;

	uint8_t buffer[11 + NET_TICKS_PER_PACKET * NET_MAX_SLOTS * NET_INPUT_SIZE];
	uint8_t *p = buffer;
	p = put_u8(p, NET_R_BUNDLE);
	p = put_u32(p, c->input_contig);
	p = put_u32(p, first);
	p = put_u8(p, count);
	p = put_u8(p, room->players);
	for (uint32_t t = first; t < first + count; ++t)
		for (unsigned int s = 0; s < room->players; ++s)
			p = put_bytes(p, room->inputs[t % RELAY_TICK_RING][s], NET_INPUT_SIZE);

	send_data(c->peer, NET_CHANNEL_TICKS, buffer, p - buffer);
}

/** Makes every bundle whose inputs have all arrived; returns whether it made any. */
static bool make_bundles(Room *room)
{
	bool made = false;

	while (!room->stopped)
	{
		const uint32_t t = room->next_tick;

		FOR_EACH_CLIENT(room, c)
		{
			if (c->input_contig < t)
				return made;
		}

		// Each input carries the hash of the sender's newest tick; all of them must agree.
		const TickHash *first = NULL;
		bool agree = true;
		FOR_EACH_CLIENT(room, c)
		{
			const TickHash *h = &room->hashes[t % RELAY_TICK_RING][c->first_slot];
			if (first == NULL)
				first = h;
			else if (h->tick != first->tick || h->hash != first->hash)
				agree = false;
		}

		if (!agree)
		{
			relay_log("room %.6s: DESYNC at tick %u (found while bundling tick %u)",
			          room->code, (unsigned)first->tick, (unsigned)t);
			room->stopped = true;
			send_end(room, NET_END_DESYNC, 0, first->tick);
			return made;
		}

		++room->next_tick;
		made = true;
	}

	return made;
}

static uint32_t oldest_unacked(const Room *room)
{
	uint32_t oldest = room->next_tick;
	FOR_EACH_CLIENT(room, c)
	{
		if (c->bundle_ack + 1 < oldest)
			oldest = c->bundle_ack + 1;
	}
	return oldest;
}

static void end_game_if_done(Room *room)
{
	if (!room->in_game)
		return;

	FOR_EACH_CLIENT(room, c)
	{
		if (!c->done)
			return;
	}

	relay_log("room %.6s: game over after %u ticks", room->code, (unsigned)(room->next_tick - 1));
	room->in_game = false;
	room->stopped = false;
	send_lobby(room);
}

static void leave_room(Client *c)
{
	Room *room = c->room;
	if (room == NULL)
		return;

	relay_log("room %.6s: %s left", room->code, c->name);

	const unsigned int old_slot = c->first_slot;
	const bool was_host = room->host_slot == c->first_slot;

	// close the gap, keeping everyone else in order
	Client *remaining[NET_MAX_SLOTS];
	unsigned int n = 0;
	for (unsigned int i = 0; i < NET_MAX_SLOTS; ++i)
	{
		if (room->slots[i] != NULL && room->slots[i] != c)
			remaining[n++] = room->slots[i];
		room->slots[i] = NULL;
	}

	unsigned int new_host_slot = 0;  // the first player takes over if the host left
	for (unsigned int i = 0; i < n; ++i)
	{
		Client *owner = remaining[i];
		if (i == 0 || remaining[i - 1] != owner)
		{
			if (!was_host && owner->first_slot == room->host_slot)
				new_host_slot = i;
			owner->first_slot = i;
		}
		room->slots[i] = owner;
	}
	room->host_slot = new_host_slot;

	c->room = NULL;

	if (n == 0)
	{
		relay_log("room %.6s: closed", room->code);
		room->used = false;
		return;
	}

	if (room->in_game && !room->stopped)
	{
		room->stopped = true;
		send_end(room, NET_END_PLAYER_LEFT, old_slot, 0);
	}

	end_game_if_done(room);
	send_lobby(room);
}

/* --- messages --- */

static void copy_name(char *dst, const uint8_t *src)
{
	for (size_t i = 0; i < NET_NAME_LEN - 1; ++i)
	{
		const char ch = (char)src[i];
		dst[i] = (ch >= 32 && ch < 127) ? ch : (ch == '\0' ? '\0' : '?');
		if (ch == '\0')
			break;
	}
	dst[NET_NAME_LEN - 1] = '\0';
	if (dst[0] == '\0')
		strcpy(dst, "PLAYER");
}

static void handle_hello(Relay *relay, Client *c, Reader *r)
{
	const uint32_t magic = get_u32(r);
	const uint16_t version = get_u16(r);
	const uint8_t action = get_u8(r);
	const uint8_t local = get_u8(r);
	const uint8_t max_players = get_u8(r);
	const uint8_t *code = get_bytes(r, NET_CODE_LEN);
	const uint8_t *name = get_bytes(r, NET_NAME_LEN);

	if (!r->ok || magic != NET_MAGIC || c->room != NULL)
	{
		send_reject(c->peer, NET_REJECT_BAD_REQUEST);
		return;
	}
	if (version != NET_PROTOCOL_VERSION)
	{
		send_reject(c->peer, NET_REJECT_VERSION);
		return;
	}
	if (local < 1 || local > NET_MAX_LOCAL)
	{
		send_reject(c->peer, NET_REJECT_BAD_REQUEST);
		return;
	}

	memset(c->name, 0, sizeof(c->name));
	copy_name(c->name, name);
	c->slot_count = local;

	Room *room = NULL;

	if (action == NET_HELLO_CREATE)
	{
		if (max_players < local || max_players > NET_MAX_SLOTS)
		{
			send_reject(c->peer, NET_REJECT_BAD_REQUEST);
			return;
		}

		for (size_t i = 0; i < RELAY_MAX_ROOMS && room == NULL; ++i)
			if (!relay->rooms[i].used)
				room = &relay->rooms[i];
		if (room == NULL)
		{
			send_reject(c->peer, NET_REJECT_SERVER_FULL);
			return;
		}

		memset(room, 0, sizeof(*room));
		room->used = true;
		room->created_at = enet_time_get();
		room->max_players = max_players;
		room->episode = 1;
		room->difficulty = 2;
		room->game_speed = 3;
		room->detail = 2;

		// a fresh code, different from every open room's
		for (bool unique = false; !unique; )
		{
			for (size_t i = 0; i < NET_CODE_LEN; ++i)
				room->code[i] = NET_CODE_ALPHABET[next_random(relay) % (sizeof(NET_CODE_ALPHABET) - 1)];

			unique = true;
			for (size_t i = 0; i < RELAY_MAX_ROOMS; ++i)
				if (&relay->rooms[i] != room && relay->rooms[i].used && memcmp(relay->rooms[i].code, room->code, NET_CODE_LEN) == 0)
					unique = false;
		}

		relay_log("room %.6s: created by %s (%s)", room->code, c->name, peer_ip(c->peer));
	}
	else if (action == NET_HELLO_JOIN)
	{
		char wanted[NET_CODE_LEN];
		for (size_t i = 0; i < NET_CODE_LEN; ++i)
			wanted[i] = (code[i] >= 'a' && code[i] <= 'z') ? code[i] - 'a' + 'A' : (char)code[i];

		for (size_t i = 0; i < RELAY_MAX_ROOMS && room == NULL; ++i)
			if (relay->rooms[i].used && memcmp(relay->rooms[i].code, wanted, NET_CODE_LEN) == 0)
				room = &relay->rooms[i];

		if (room == NULL)
		{
			send_reject(c->peer, NET_REJECT_NO_ROOM);
			return;
		}
		if (room->in_game)
		{
			send_reject(c->peer, NET_REJECT_IN_GAME);
			return;
		}
		if (room_used_slots(room) + local > room->max_players)
		{
			send_reject(c->peer, NET_REJECT_ROOM_FULL);
			return;
		}

		relay_log("room %.6s: %s joined (%s)", room->code, c->name, peer_ip(c->peer));
	}
	else
	{
		send_reject(c->peer, NET_REJECT_BAD_REQUEST);
		return;
	}

	const unsigned int first = room_used_slots(room);
	for (unsigned int i = 0; i < local; ++i)
		room->slots[first + i] = c;
	c->first_slot = first;
	c->room = room;

	uint8_t welcome[1 + NET_CODE_LEN];
	welcome[0] = NET_R_WELCOME;
	memcpy(welcome + 1, room->code, NET_CODE_LEN);
	send_data(c->peer, NET_CHANNEL_CONTROL, welcome, sizeof(welcome));

	send_lobby(room);
}

static bool is_host(const Client *c)
{
	return c->room != NULL && c->room->host_slot == c->first_slot;
}

static void handle_settings(Client *c, Reader *r)
{
	const uint8_t episode = get_u8(r);
	const uint8_t difficulty = get_u8(r);
	const uint8_t game_speed = get_u8(r);
	const uint8_t detail = get_u8(r);

	if (!r->ok || !is_host(c) || c->room->in_game)
		return;
	if (episode < 1 || episode > NET_MAX_EPISODE || difficulty < NET_MIN_DIFFICULTY || difficulty > NET_MAX_DIFFICULTY ||
	    game_speed < 1 || game_speed > NET_MAX_GAME_SPEED || detail < 1 || detail > NET_MAX_DETAIL)
		return;

	Room *room = c->room;
	room->episode = episode;
	room->difficulty = difficulty;
	room->game_speed = game_speed;
	room->detail = detail;
	send_lobby(room);
}

static void handle_start(Client *c)
{
	if (!is_host(c) || c->room->in_game)
		return;

	Room *room = c->room;

	// the input delay must cover a round trip through the relay, or every tick waits for it
	enet_uint32 max_rtt = 0;
	FOR_EACH_CLIENT(room, other)
	{
		if (other->peer->roundTripTime > max_rtt)
			max_rtt = other->peer->roundTripTime;
	}
	unsigned int delay = 1 + (max_rtt + RELAY_TICK_MS - 1) / RELAY_TICK_MS;
	if (delay < RELAY_MIN_DELAY)
		delay = RELAY_MIN_DELAY;
	if (delay > RELAY_MAX_DELAY)
		delay = RELAY_MAX_DELAY;

	room->in_game = true;
	room->stopped = false;
	room->players = room_used_slots(room);
	room->next_tick = 1;
	room->last_resume_tick = 0;
	memset(room->inputs, 0, sizeof(room->inputs));
	memset(room->hashes, 0, sizeof(room->hashes));

	FOR_EACH_CLIENT(room, other)
	{
		other->input_contig = 0;
		other->bundle_ack = 0;
		other->done = false;
	}

	relay_log("room %.6s: game started, %u players, input delay %u (round trip up to %u ms)",
	          room->code, room->players, delay, (unsigned)max_rtt);

	const uint8_t data[7] = { NET_R_START, room->players, delay, room->episode, room->difficulty, room->game_speed, room->detail };
	broadcast(room, NET_CHANNEL_CONTROL, data, sizeof(data));
	send_lobby(room);
}

static void handle_input(Client *c, Reader *r)
{
	Room *room = c->room;
	if (room == NULL || !room->in_game || c->done)
		return;

	const uint32_t bundle_ack = get_u32(r);
	const uint32_t first = get_u32(r);
	const uint8_t count = get_u8(r);
	if (!r->ok)
		return;

	if (bundle_ack > c->bundle_ack && bundle_ack < room->next_tick)
		c->bundle_ack = bundle_ack;

	const uint32_t limit = oldest_unacked(room) + RELAY_TICK_RING - 1;  // don't overwrite what may be resent

	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t t = first + i;
		const uint32_t hash_tick = get_u32(r);
		const uint64_t hash = get_u64(r);
		const uint8_t *inputs = get_bytes(r, (size_t)c->slot_count * NET_INPUT_SIZE);
		if (!r->ok)
			break;

		if (t <= c->input_contig)
			continue;  // already have it
		if (t != c->input_contig + 1 || t >= limit || room->stopped)
			break;

		const size_t index = t % RELAY_TICK_RING;
		for (unsigned int s = 0; s < c->slot_count; ++s)
			memcpy(room->inputs[index][c->first_slot + s], inputs + s * NET_INPUT_SIZE, NET_INPUT_SIZE);
		room->hashes[index][c->first_slot].tick = hash_tick;
		room->hashes[index][c->first_slot].hash = hash;
		c->input_contig = t;
	}

	if (make_bundles(room))
	{
		FOR_EACH_CLIENT(room, other)
			send_bundles(room, other);
	}
	else
	{
		send_bundles(room, c);  // what it's still missing (it resends while it waits)
	}
}

static void handle_resume(Client *c, Reader *r)
{
	Room *room = c->room;
	const uint32_t tick = get_u32(r);
	const uint8_t quit = get_u8(r);
	if (!r->ok || room == NULL || !room->in_game)
		return;

	// every computer holds at the same tick; the first to let go releases all of them
	if (tick <= room->last_resume_tick)
		return;
	room->last_resume_tick = tick;

	uint8_t data[7];
	uint8_t *p = data;
	p = put_u8(p, NET_R_RESUME);
	p = put_u32(p, tick);
	p = put_u8(p, quit != 0);
	p = put_u8(p, c->first_slot);
	broadcast(room, NET_CHANNEL_CONTROL, data, p - data);
}

static void handle_done(Client *c)
{
	Room *room = c->room;
	if (room == NULL || !room->in_game)
		return;

	// Everyone ends a game at the same tick by themselves, so anyone still playing will
	// get there anyway; if not, this computer has left the game like a disconnect would.
	if (!room->stopped)
	{
		room->stopped = true;
		send_end(room, NET_END_PLAYER_LEFT, c->first_slot, 0);
	}

	c->done = true;
	end_game_if_done(room);
}

static void handle_packet(Relay *relay, Client *c, const ENetPacket *packet)
{
	const enet_uint32 now = enet_time_get();
	if (now - c->rate_start >= 1000)
	{
		c->rate_start = now;
		c->rate_count = 0;
	}
	if (++c->rate_count > RELAY_MAX_RATE || packet->dataLength > RELAY_MAX_PACKET)
	{
		relay_log("dropping %s: too much traffic", peer_ip(c->peer));
		enet_peer_disconnect(c->peer, 0);
		return;
	}

	Reader r = { packet->data, packet->data + packet->dataLength, true };
	const uint8_t type = get_u8(&r);

	if (c->rejected || (c->room == NULL && type != NET_C_HELLO))
		return;

	switch (type)
	{
	case NET_C_HELLO:    handle_hello(relay, c, &r); break;
	case NET_C_SETTINGS: handle_settings(c, &r); break;
	case NET_C_START:    handle_start(c); break;
	case NET_C_INPUT:    handle_input(c, &r); break;
	case NET_C_RESUME:   handle_resume(c, &r); break;
	case NET_C_DONE:     handle_done(c); break;
	default:             break;
	}
}

/* --- LAN discovery --- */

/** Private, loopback and link-local IPv4 addresses: the only ones a LAN game answers. */
static bool is_lan_address(const ENetAddress *address)
{
	const uint8_t *ip = (const uint8_t *)&address->host;  // network byte order
	return ip[0] == 10 || ip[0] == 127 ||
	       (ip[0] == 172 && (ip[1] & 0xf0) == 16) ||
	       (ip[0] == 192 && ip[1] == 168) ||
	       (ip[0] == 169 && ip[1] == 254);
}

static void answer_discovery(Relay *relay)
{
	if (relay->discovery == ENET_SOCKET_NULL)
		return;

	for (int n = 0; n < 16; ++n)
	{
		uint8_t query[64];
		ENetAddress from;
		ENetBuffer buffer = { .data = query, .dataLength = sizeof(query) };
		const int length = enet_socket_receive(relay->discovery, &from, &buffer, 1);
		if (length <= 0)
			return;

		if (length != NET_DISCOVERY_QUERY_SIZE || memcmp(query, "TYR?", 4) != 0 ||
		    (query[4] | (query[5] << 8)) != NET_PROTOCOL_VERSION || !is_lan_address(&from))
			continue;

		// one answer per query, so a spoofed query can't be turned into a flood
		for (size_t i = 0, answers = 0; i < RELAY_MAX_ROOMS && answers < 1; ++i)
		{
			const Room *room = &relay->rooms[i];
			if (!room->used || room->slots[room->host_slot] == NULL)
				continue;

			uint8_t answer[NET_DISCOVERY_ANSWER_SIZE];
			uint8_t *p = answer;
			p = put_bytes(p, "TYR!", 4);
			p = put_u16(p, NET_PROTOCOL_VERSION);
			p = put_u16(p, relay->port);
			p = put_bytes(p, room->code, NET_CODE_LEN);
			p = put_bytes(p, room->slots[room->host_slot]->name, NET_NAME_LEN);
			p = put_u8(p, room_used_slots(room));
			p = put_u8(p, room->max_players);
			p = put_u8(p, room->in_game);

			ENetBuffer reply = { .data = answer, .dataLength = p - answer };
			enet_socket_send(relay->discovery, &from, &reply, 1);
			++answers;
		}
	}
}

/* --- relay --- */

Relay *relay_create(uint16_t port, bool lan)
{
	Relay *relay = calloc(1, sizeof(*relay));
	if (relay == NULL)
		return NULL;

	ENetAddress address = { .host = ENET_HOST_ANY, .port = port };
	relay->host = enet_host_create(&address, RELAY_MAX_PEERS, NET_CHANNELS, 0, 0);
	if (relay->host == NULL)
	{
		relay_log("can't open UDP port %u", (unsigned)port);
		free(relay);
		return NULL;
	}

	// ENet's own limits are 32 MB, and it buffers a packet before the relay sees it
	relay->host->maximumPacketSize = RELAY_MAX_PACKET;
	relay->host->maximumWaitingData = 8 * RELAY_MAX_PACKET;
	relay->host->duplicatePeers = RELAY_MAX_PER_IP;

	relay->port = port;
	relay->rng = ((uint64_t)time(NULL) << 32) ^ enet_time_get() ^ (uint64_t)(uintptr_t)relay ^ 0x9E3779B97F4A7C15ull;
	relay->discovery = ENET_SOCKET_NULL;

	if (lan)
	{
		relay->discovery = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
		ENetAddress discovery_address = { .host = ENET_HOST_ANY, .port = NET_DISCOVERY_PORT };
		if (relay->discovery != ENET_SOCKET_NULL)
		{
			enet_socket_set_option(relay->discovery, ENET_SOCKOPT_REUSEADDR, 1);
			enet_socket_set_option(relay->discovery, ENET_SOCKOPT_NONBLOCK, 1);
			if (enet_socket_bind(relay->discovery, &discovery_address) != 0)
			{
				relay_log("can't open UDP port %u; LAN players won't see this game", (unsigned)NET_DISCOVERY_PORT);
				enet_socket_destroy(relay->discovery);
				relay->discovery = ENET_SOCKET_NULL;
			}
		}
	}

	relay_log("listening on UDP port %u%s", (unsigned)port, relay->discovery != ENET_SOCKET_NULL ? " (LAN discovery on)" : "");
	return relay;
}

static void drop_idle_connections(Relay *relay)
{
	const enet_uint32 now = enet_time_get();
	if (now - relay->last_check < 1000)
		return;
	relay->last_check = now;

	for (size_t i = 0; i < relay->host->peerCount; ++i)
	{
		ENetPeer *peer = &relay->host->peers[i];
		const Client *c = peer->data;
		if (peer->state != ENET_PEER_STATE_CONNECTED || c == NULL)
			continue;

		// joined nothing in time, or sitting in a room for longer than anyone plays
		if ((c->room == NULL && now - c->connected_at > RELAY_HELLO_TIMEOUT) ||
		    (c->room != NULL && now - c->room->created_at > RELAY_ROOM_LIFETIME))
		{
			enet_peer_disconnect(peer, 0);
		}
	}
}

void relay_service(Relay *relay, uint32_t timeout_ms)
{
	answer_discovery(relay);

	ENetEvent event;
	int result = enet_host_service(relay->host, &event, timeout_ms);
	while (result > 0)
	{
		switch (event.type)
		{
		case ENET_EVENT_TYPE_CONNECT:
		{
			unsigned int same_ip = 0;
			for (size_t i = 0; i < relay->host->peerCount; ++i)
			{
				const ENetPeer *other = &relay->host->peers[i];
				if (other->data != NULL && other->address.host == event.peer->address.host)
					++same_ip;
			}

			Client *c = calloc(1, sizeof(*c));
			if (c == NULL || same_ip >= RELAY_MAX_PER_IP)
			{
				free(c);
				enet_peer_disconnect(event.peer, 0);
				break;
			}

			c->peer = event.peer;
			c->connected_at = enet_time_get();
			c->rate_start = c->connected_at;
			event.peer->data = c;
			break;
		}
		case ENET_EVENT_TYPE_RECEIVE:
		{
			Client *c = event.peer->data;
			if (c != NULL)
				handle_packet(relay, c, event.packet);
			enet_packet_destroy(event.packet);
			break;
		}
		case ENET_EVENT_TYPE_DISCONNECT:
		{
			Client *c = event.peer->data;
			if (c != NULL)
			{
				leave_room(c);
				free(c);
				event.peer->data = NULL;
			}
			break;
		}
		default:
			break;
		}

		result = enet_host_service(relay->host, &event, 0);
	}

	answer_discovery(relay);
	drop_idle_connections(relay);
	enet_host_flush(relay->host);
}

void relay_destroy(Relay *relay)
{
	if (relay == NULL)
		return;

	for (size_t i = 0; i < relay->host->peerCount; ++i)
	{
		ENetPeer *peer = &relay->host->peers[i];
		free(peer->data);
		peer->data = NULL;
		if (peer->state != ENET_PEER_STATE_DISCONNECTED)
			enet_peer_disconnect_now(peer, 0);
	}

	enet_host_destroy(relay->host);
	if (relay->discovery != ENET_SOCKET_NULL)
		enet_socket_destroy(relay->discovery);
	free(relay);
}
