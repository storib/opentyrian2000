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
#include "netplay.h"

#include "config.h"
#include "episodes.h"
#include "file.h"
#include "fonthand.h"
#include "helptext.h"
#include "keyboard.h"
#include "mainint.h"
#include "params.h"
#include "player.h"
#include "relay.h"
#include "sprite.h"
#include "tyrian2.h"
#include "varz.h"
#include "vga256d.h"
#include "video.h"

#include "SDL.h"

#include <enet/enet.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_CONNECT_TIMEOUT    5000   // ms
#define NET_RESEND_MS          20     // while waiting for a bundle, resend unacknowledged input this often
#define NET_WAIT_NOTICE_MS     500    // waits shorter than this don't show "waiting for players"
#define NET_RING               256    // ticks of inputs and bundles kept; far more than are ever in flight
#define NET_MAX_RESUMES        16
#define NET_LAN_GAME_EXPIRY    3000   // ms a LAN game stays listed after its last answer

bool isNetworkGame = false;
uint thisPlayerNum = 0;

char net_player_name[NET_NAME_LEN] = "PLAYER";
char net_relay_address[128] = NET_DEFAULT_RELAY;
const char *net_name_option = NULL, *net_relay_option = NULL;

typedef struct
{
	Uint32 hash_tick;
	Uint64 hash;
	PlayerInput input[NET_MAX_LOCAL];
} LocalEntry;

typedef struct
{
	Uint32 tick;
	bool quit;
	unsigned int slot;
} Resume;

typedef struct
{
	NetLanGame game;
	Uint32 seen;
} LanEntry;

static struct
{
	bool enet_ready;
	ENetHost *host;
	ENetPeer *peer;
	NetStatus status;
	char error[128];
	Uint32 connect_started;
	bool create;
	char join_code[NET_CODE_LEN];
	char code[NET_CODE_LEN + 1];
	NetLobby lobby;

	// the game the host started
	bool start_received;
	unsigned int players, input_delay, episode, difficulty, game_speed, detail;

	// lockstep
	Uint32 tick;           // the tick being simulated; 0 before the first
	Uint32 queued_upto;    // local input exists up to this tick
	Uint32 input_acked;    // the relay has local input up to this tick
	Uint32 bundle_contig;  // every bundle up to this tick has arrived
	LocalEntry local[NET_RING];
	PlayerInput bundles[NET_RING][NET_MAX_SLOTS];
	Uint32 last_hash_tick;
	Uint64 last_hash;

	// ending
	bool ended;
	char end_message[128];
	bool stop_set;
	Uint32 stop_tick, desync_tick;
	unsigned int stop_reason, stop_slot;
	Resume resumes[NET_MAX_RESUMES];
	unsigned int resume_count;

	// the host's gameplay settings replace these during a game
	bool settings_saved;
	JE_byte saved_game_speed, saved_processor_type;
	JE_boolean saved_constant_play, saved_constant_die;

	// hosting here
	Relay *relay;
	SDL_Thread *relay_thread;
	SDL_atomic_t relay_stop;
	char local_address[64];

	// LAN discovery
	bool discovering;
	ENetSocket discovery;
	Uint32 discovery_last_query;
	LanEntry lan[NET_MAX_LAN_GAMES];
	unsigned int lan_count;
} net;

/* --- helpers --- */

static void set_error(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(net.error, sizeof(net.error), format, args);
	va_end(args);
	fprintf(stderr, "net: %s\n", net.error);
}

static bool ensure_enet(void)
{
	if (net.enet_ready)
		return true;

	if (enet_initialize() != 0)
	{
		set_error("Networking is unavailable on this computer.");
		return false;
	}
	atexit(enet_deinitialize);
	net.enet_ready = true;
	net.discovery = ENET_SOCKET_NULL;
	return true;
}

static Uint8 *put_u8(Uint8 *p, Uint8 v)   { *p++ = v; return p; }
static Uint8 *put_u16(Uint8 *p, Uint16 v) { p = put_u8(p, v & 0xff); return put_u8(p, v >> 8); }
static Uint8 *put_u32(Uint8 *p, Uint32 v) { p = put_u16(p, v & 0xffff); return put_u16(p, v >> 16); }
static Uint8 *put_u64(Uint8 *p, Uint64 v) { p = put_u32(p, v & 0xffffffff); return put_u32(p, v >> 32); }
static Uint8 *put_bytes(Uint8 *p, const void *bytes, size_t n) { memcpy(p, bytes, n); return p + n; }

typedef struct
{
	const Uint8 *p, *end;
	bool ok;
} Reader;

static Uint8 get_u8(Reader *r)
{
	if (r->p + 1 > r->end)
	{
		r->ok = false;
		return 0;
	}
	return *r->p++;
}

static Uint16 get_u16(Reader *r)
{
	Uint16 v = get_u8(r);
	v |= (Uint16)(get_u8(r) << 8);
	return v;
}

static Uint32 get_u32(Reader *r)
{
	Uint32 v = get_u16(r);
	v |= (Uint32)get_u16(r) << 16;
	return v;
}

static const Uint8 *get_bytes(Reader *r, size_t n)
{
	if ((size_t)(r->end - r->p) < n)
	{
		r->ok = false;
		return NULL;
	}
	const Uint8 *bytes = r->p;
	r->p += n;
	return bytes;
}

static void get_string(Reader *r, char *dst, size_t n)
{
	const Uint8 *src = get_bytes(r, n);
	if (src == NULL)
	{
		dst[0] = '\0';
		return;
	}
	memcpy(dst, src, n);
	dst[n - 1] = '\0';
}

static Uint8 *put_input(Uint8 *p, const PlayerInput *in)
{
	Uint32 angle;
	memcpy(&angle, &in->link_gun_angle, sizeof(angle));

	p = put_u16(p, (Uint16)in->move_x);
	p = put_u16(p, (Uint16)in->move_y);
	p = put_u32(p, (Uint32)in->analog_x);
	p = put_u32(p, (Uint32)in->analog_y);
	p = put_u8(p, in->buttons);
	p = put_u8(p, in->link_gun_analog);
	return put_u32(p, angle);
}

static void get_input(Reader *r, PlayerInput *in)
{
	// one read per statement: the order of reads must be fixed
	const Uint16 move_x = get_u16(r);
	const Uint16 move_y = get_u16(r);
	const Uint32 analog_x = get_u32(r);
	const Uint32 analog_y = get_u32(r);
	const Uint8 buttons = get_u8(r);
	const Uint8 link_gun_analog = get_u8(r);
	const Uint32 angle = get_u32(r);

	memset(in, 0, sizeof(*in));
	in->move_x = (Sint16)move_x;
	in->move_y = (Sint16)move_y;
	in->analog_x = (Sint32)analog_x;
	in->analog_y = (Sint32)analog_y;
	in->buttons = buttons;
	in->link_gun_analog = link_gun_analog != 0;
	memcpy(&in->link_gun_angle, &angle, sizeof(angle));
}

static void send_data(int channel, const Uint8 *data, size_t length)
{
	if (net.peer == NULL || net.status == NET_CONNECTING)
		return;

	const enet_uint32 flags = channel == NET_CHANNEL_CONTROL ? ENET_PACKET_FLAG_RELIABLE : 0;
	ENetPacket *packet = enet_packet_create(data, length, flags);
	if (packet != NULL && enet_peer_send(net.peer, (enet_uint8)channel, packet) != 0)
		enet_packet_destroy(packet);
	enet_host_flush(net.host);
}

static bool is_local_slot(unsigned int slot)
{
	return slot >= net.lobby.my_slot && slot < net.lobby.my_slot + net.lobby.my_slot_count;
}

/* --- connection --- */

static void close_connection(void)
{
	if (net.host != NULL)
	{
		if (net.peer != NULL && net.peer->state == ENET_PEER_STATE_CONNECTED)
		{
			// tell the relay now, so the others aren't left waiting for a time-out
			enet_peer_disconnect(net.peer, 0);
			ENetEvent event;
			const Uint32 start = SDL_GetTicks();
			while (SDL_GetTicks() - start < 300 && enet_host_service(net.host, &event, 20) >= 0)
			{
				if (event.type == ENET_EVENT_TYPE_RECEIVE)
					enet_packet_destroy(event.packet);
				else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
					break;
			}
		}
		enet_host_destroy(net.host);
	}

	net.host = NULL;
	net.peer = NULL;
	net.status = NET_OFFLINE;
	net.start_received = false;
}

static void stop_relay(void)
{
	if (net.relay_thread != NULL)
	{
		SDL_AtomicSet(&net.relay_stop, 1);
		SDL_WaitThread(net.relay_thread, NULL);
		net.relay_thread = NULL;
	}
	relay_destroy(net.relay);
	net.relay = NULL;
	net.local_address[0] = '\0';
}

/** Ends the game being played right away; the level loop sees net_game_ended(). */
static void end_now(const char *message)
{
	if (!isNetworkGame || net.ended)
		return;

	net.ended = true;
	if (message != NULL)
		SDL_strlcpy(net.end_message, message, sizeof(net.end_message));
	reallyEndLevel = true;
	playerEndLevel = true;
}

static void connection_lost(void)
{
	if (net.error[0] == '\0')
		set_error(net.status == NET_CONNECTING ? "Couldn't reach the server." : "Lost the connection to the server.");
	close_connection();
	end_now(net.error);
}

bool net_connect(const char *address, uint16_t default_port, const char *code)
{
	if (net.host != NULL)
		close_connection();

	net.error[0] = '\0';
	net.end_message[0] = '\0';

	if (!ensure_enet())
		return false;

	// "host" or "host:port"
	char host_name[128];
	SDL_strlcpy(host_name, address, sizeof(host_name));
	unsigned long port = default_port;
	char *colon = strrchr(host_name, ':');
	if (colon != NULL)
	{
		*colon = '\0';
		port = strtoul(colon + 1, NULL, 10);
		if (port == 0 || port > 65535)
			port = default_port;
	}

	ENetAddress enet_address;
	if (enet_address_set_host(&enet_address, host_name) != 0)
	{
		set_error("Can't find the server %s.", host_name);
		return false;
	}
	enet_address.port = (enet_uint16)port;

	net.host = enet_host_create(NULL, 1, NET_CHANNELS, 0, 0);
	if (net.host == NULL)
	{
		set_error("Networking is unavailable on this computer.");
		return false;
	}

	// nothing the relay sends is anywhere near this; ENet would accept up to 32 MB
	net.host->maximumPacketSize = 4096;
	net.host->maximumWaitingData = 64 * 1024;

	net.peer = enet_host_connect(net.host, &enet_address, NET_CHANNELS, 0);
	if (net.peer == NULL)
	{
		set_error("Networking is unavailable on this computer.");
		close_connection();
		return false;
	}

	net.status = NET_CONNECTING;
	net.connect_started = SDL_GetTicks();
	net.create = code == NULL;
	memset(net.join_code, 0, sizeof(net.join_code));
	if (code != NULL)
		memcpy(net.join_code, code, SDL_min(strlen(code), sizeof(net.join_code)));
	memset(&net.lobby, 0, sizeof(net.lobby));
	net.code[0] = '\0';
	net.start_received = false;

	fprintf(stderr, "net: connecting to %s:%lu\n", host_name, port);
	return true;
}

static int relay_thread_main(void *data)
{
	(void)data;
	while (!SDL_AtomicGet(&net.relay_stop))
		relay_service(net.relay, 5);
	return 0;
}

/** Finds this computer's LAN address: the one it would use to reach the internet. */
static void find_local_address(void)
{
	net.local_address[0] = '\0';

	ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	if (s == ENET_SOCKET_NULL)
		return;

	ENetAddress remote = { .host = 0, .port = 53 };
	enet_address_set_host_ip(&remote, "8.8.8.8");  // connecting a UDP socket sends nothing
	ENetAddress local;
	if (enet_socket_connect(s, &remote) == 0 && enet_socket_get_address(s, &local) == 0)
		enet_address_get_host_ip(&local, net.local_address, sizeof(net.local_address));
	enet_socket_destroy(s);
}

bool net_host_here(void)
{
	if (!ensure_enet())
		return false;

	stop_relay();

	// the usual port, or the next ones if another program (or a test relay) has it
	static const uint16_t ports[] = { NET_DEFAULT_PORT, 21910, 21911, 21912, 21913 };
	uint16_t port = 0;
	for (size_t i = 0; i < COUNTOF(ports) && net.relay == NULL; ++i)
	{
		net.relay = relay_create(ports[i], true);
		port = ports[i];
	}
	if (net.relay == NULL)
	{
		set_error("Can't host: the network ports are in use.");
		return false;
	}

	SDL_AtomicSet(&net.relay_stop, 0);
	net.relay_thread = SDL_CreateThread(relay_thread_main, "relay", NULL);
	if (net.relay_thread == NULL)
	{
		set_error("Can't host: %s", SDL_GetError());
		stop_relay();
		return false;
	}

	find_local_address();

	char address[32];
	snprintf(address, sizeof(address), "127.0.0.1:%u", (unsigned)port);
	if (!net_connect(address, port, NULL))
	{
		stop_relay();
		return false;
	}
	return true;
}

static void restore_settings(void);

void net_leave(void)
{
	close_connection();
	stop_relay();
	end_now(NULL);
	restore_settings();  // also before JE_tyrianHalt() saves them
}

static void send_hello(void)
{
	Uint8 data[64];
	Uint8 *p = data;
	char name[NET_NAME_LEN] = { 0 };
	SDL_strlcpy(name, net_current_name(), sizeof(name));

	p = put_u8(p, NET_C_HELLO);
	p = put_u32(p, NET_MAGIC);
	p = put_u16(p, NET_PROTOCOL_VERSION);
	p = put_u8(p, net.create ? NET_HELLO_CREATE : NET_HELLO_JOIN);
	p = put_u8(p, 1);  // players on this computer
	p = put_u8(p, 2);  // room size: the game has 2 player slots for now
	p = put_bytes(p, net.join_code, NET_CODE_LEN);
	p = put_bytes(p, name, NET_NAME_LEN);

	// sent as soon as the connection is up, before the status says so
	ENetPacket *packet = enet_packet_create(data, p - data, ENET_PACKET_FLAG_RELIABLE);
	if (packet != NULL && enet_peer_send(net.peer, NET_CHANNEL_CONTROL, packet) != 0)
		enet_packet_destroy(packet);
}

/* --- receiving --- */

/** Clears the lockstep state for a new game. */
static void reset_game_state(void)
{
	net.tick = 0;
	net.queued_upto = 0;
	net.input_acked = 0;
	net.bundle_contig = 0;
	net.last_hash_tick = 0;
	net.last_hash = 0;
	net.stop_set = false;
	net.resume_count = 0;
	memset(net.local, 0, sizeof(net.local));
	memset(net.bundles, 0, sizeof(net.bundles));
}

static void handle_bundle(Reader *r)
{
	if (net.status != NET_IN_GAME)
		return;

	const Uint32 input_ack = get_u32(r);
	const Uint32 first = get_u32(r);
	const Uint8 count = get_u8(r);
	const Uint8 players = get_u8(r);
	if (!r->ok || players != net.players)
		return;

	if (input_ack > net.input_acked && input_ack <= net.queued_upto)
		net.input_acked = input_ack;

	for (Uint32 i = 0; i < count; ++i)
	{
		const Uint32 t = first + i;
		PlayerInput inputs[NET_MAX_SLOTS];
		for (unsigned int s = 0; s < players; ++s)
			get_input(r, &inputs[s]);
		if (!r->ok)
			break;

		if (t <= net.bundle_contig)
			continue;
		if (t != net.bundle_contig + 1 || t >= net.tick + NET_RING)
			break;

		memcpy(net.bundles[t % NET_RING], inputs, sizeof(inputs[0]) * players);
		net.bundle_contig = t;
	}
}

static void handle_lobby(Reader *r)
{
	NetLobby lobby;
	memset(&lobby, 0, sizeof(lobby));
	lobby.in_game = get_u8(r);
	lobby.host_slot = get_u8(r);
	lobby.max_players = get_u8(r);
	lobby.my_slot = get_u8(r);
	lobby.my_slot_count = get_u8(r);
	lobby.episode = get_u8(r);
	lobby.difficulty = get_u8(r);
	lobby.game_speed = get_u8(r);
	lobby.detail = get_u8(r);
	for (unsigned int i = 0; i < NET_MAX_SLOTS; ++i)
	{
		lobby.used[i] = get_u8(r);
		get_string(r, lobby.names[i], NET_NAME_LEN);
	}
	// never trust the relay with sizes: this computer has the 1 player it asked for
	if (!r->ok || lobby.max_players < 1 || lobby.max_players > NET_MAX_SLOTS ||
	    lobby.my_slot >= lobby.max_players || lobby.host_slot >= lobby.max_players || lobby.my_slot_count != 1)
		return;

	// slots move when someone leaves; a game already running keeps the ones it started with
	if (net.status == NET_IN_GAME)
	{
		lobby.my_slot = net.lobby.my_slot;
		lobby.my_slot_count = net.lobby.my_slot_count;
		memcpy(lobby.used, net.lobby.used, sizeof(lobby.used));
		memcpy(lobby.names, net.lobby.names, sizeof(lobby.names));
	}
	net.lobby = lobby;
}

static void handle_packet(const ENetPacket *packet)
{
	Reader r = { packet->data, packet->data + packet->dataLength, true };

	switch (get_u8(&r))
	{
	case NET_R_WELCOME:
	{
		const Uint8 *code = get_bytes(&r, NET_CODE_LEN);
		if (code == NULL)
			break;
		memcpy(net.code, code, NET_CODE_LEN);
		net.code[NET_CODE_LEN] = '\0';
		net.status = NET_IN_LOBBY;
		fprintf(stderr, "net: in room %s\n", net.code);
		break;
	}

	case NET_R_REJECT:
		switch (get_u8(&r))
		{
		case NET_REJECT_VERSION:     set_error("The server runs a different version of the game."); break;
		case NET_REJECT_NO_ROOM:     set_error("There is no game with that code."); break;
		case NET_REJECT_ROOM_FULL:   set_error("That game is full."); break;
		case NET_REJECT_IN_GAME:     set_error("That game has already started."); break;
		case NET_REJECT_SERVER_FULL: set_error("The server is full. Try again later."); break;
		default:                     set_error("The server refused the connection."); break;
		}
		break;

	case NET_R_LOBBY:
		handle_lobby(&r);
		break;

	case NET_R_START:
	{
		const Uint8 players = get_u8(&r);
		const Uint8 delay = get_u8(&r);
		const Uint8 episode = get_u8(&r);
		const Uint8 difficulty = get_u8(&r);
		const Uint8 game_speed = get_u8(&r);
		const Uint8 detail = get_u8(&r);
		if (!r.ok || net.status != NET_IN_LOBBY || players < 1 || players > 2 || delay < 1 || delay > 32 ||
		    episode < 1 || episode > EPISODE_AVAILABLE || difficulty < DIFFICULTY_EASY || difficulty > DIFFICULTY_HARD ||
		    net.lobby.my_slot >= players)
			break;

		net.players = players;
		net.input_delay = delay;
		net.episode = episode;
		net.difficulty = difficulty;
		net.game_speed = game_speed;
		net.detail = detail;
		net.start_received = true;

		// In game from here: a stop or a resume may arrive before net_begin_game() runs,
		// and the slots the game started with mustn't move.
		net.status = NET_IN_GAME;
		reset_game_state();
		break;
	}

	case NET_R_BUNDLE:
		handle_bundle(&r);
		break;

	case NET_R_RESUME:
	{
		Resume resume;
		resume.tick = get_u32(&r);
		resume.quit = get_u8(&r) != 0;
		resume.slot = get_u8(&r);
		if (!r.ok || net.status != NET_IN_GAME)
			break;
		if (net.resume_count == NET_MAX_RESUMES)
		{
			memmove(&net.resumes[0], &net.resumes[1], sizeof(net.resumes[0]) * (NET_MAX_RESUMES - 1));
			--net.resume_count;
		}
		net.resumes[net.resume_count++] = resume;
		break;
	}

	case NET_R_END:
	{
		const Uint8 reason = get_u8(&r);
		const Uint8 slot = get_u8(&r);
		const Uint32 tick = get_u32(&r);
		const Uint32 stop_tick = get_u32(&r);
		if (!r.ok || net.status != NET_IN_GAME || net.stop_set)
			break;

		net.stop_set = true;
		net.stop_reason = reason;
		net.stop_slot = slot;
		net.desync_tick = tick;
		net.stop_tick = stop_tick;
		break;
	}

	default:
		break;
	}
}

static void handle_event(const ENetEvent *event)
{
	switch (event->type)
	{
	case ENET_EVENT_TYPE_CONNECT:
		send_hello();
		break;
	case ENET_EVENT_TYPE_RECEIVE:
		handle_packet(event->packet);
		enet_packet_destroy(event->packet);
		break;
	case ENET_EVENT_TYPE_DISCONNECT:
		net.peer = NULL;
		connection_lost();
		break;
	default:
		break;
	}
}

/** Handles traffic, waiting up to `timeout_ms` for the first of it. */
static void net_service(Uint32 timeout_ms)
{
	if (net.host == NULL)
		return;

	ENetEvent event;
	int result = enet_host_service(net.host, &event, timeout_ms);
	while (result > 0 && net.host != NULL)
	{
		handle_event(&event);
		if (net.host == NULL)
			break;
		result = enet_host_service(net.host, &event, 0);
	}

	if (net.host != NULL && net.status == NET_CONNECTING && SDL_GetTicks() - net.connect_started > NET_CONNECT_TIMEOUT)
		connection_lost();
}

void net_poll(void)
{
	net_service(0);
}

/* --- lobby --- */

NetStatus net_status(void)          { return net.status; }
const char *net_error(void)         { return net.error; }
const char *net_room_code(void)     { return net.code; }
const NetLobby *net_lobby(void)     { return &net.lobby; }
bool net_hosting_here(void)         { return net.relay != NULL; }
const char *net_local_address(void) { return net.local_address; }
bool net_start_received(void)       { return net.start_received; }
const char *net_end_message(void)   { return net.end_message; }
bool net_game_ended(void)           { return net.ended; }

void net_set_end_message(const char *message)
{
	SDL_strlcpy(net.end_message, message, sizeof(net.end_message));
}

bool net_is_host(void)
{
	return net.status >= NET_IN_LOBBY && net.lobby.host_slot == net.lobby.my_slot;
}

unsigned int net_lobby_players(void)
{
	unsigned int n = 0;
	for (unsigned int i = 0; i < NET_MAX_SLOTS; ++i)
		n += net.lobby.used[i];
	return n;
}

const char *net_player_name_of(unsigned int player_num)
{
	if (player_num < 1 || player_num > NET_MAX_SLOTS || !net.lobby.used[player_num - 1])
		return "";
	return net.lobby.names[player_num - 1];
}

void net_send_settings(unsigned int episode, unsigned int difficulty)
{
	const Uint8 data[5] = { NET_C_SETTINGS, episode, difficulty, gameSpeed, processorType };
	send_data(NET_CHANNEL_CONTROL, data, sizeof(data));
}

void net_send_start(void)
{
	const Uint8 data[1] = { NET_C_START };
	send_data(NET_CHANNEL_CONTROL, data, sizeof(data));
}

/* --- game --- */

/** Puts back this computer's own settings after a network game used the host's. */
static void restore_settings(void)
{
	if (!net.settings_saved)
		return;

	gameSpeed = net.saved_game_speed;
	processorType = net.saved_processor_type;
	JE_initProcessorType();
	constantPlay = net.saved_constant_play;
	constantDie = net.saved_constant_die;
	net.settings_saved = false;
}

void net_begin_game(void)
{
	net.start_received = false;
	isNetworkGame = true;
	thisPlayerNum = net.lobby.my_slot + 1;
	net.ended = false;
	net.end_message[0] = '\0';

	// Settings that change the game must match on every computer: use the host's, and
	// none of the testing switches (--constant, --death).
	net.saved_game_speed = gameSpeed;
	net.saved_processor_type = processorType;
	net.saved_constant_play = constantPlay;
	net.saved_constant_die = constantDie;
	net.settings_saved = true;
	if (net.game_speed >= 1 && net.game_speed <= NET_MAX_GAME_SPEED)
		gameSpeed = net.game_speed;
	if (net.detail >= 1 && net.detail <= NET_MAX_DETAIL)
		processorType = net.detail;
	JE_initProcessorType();
	constantPlay = false;
	constantDie = false;

	superTyrian = false;
	superArcadeMode = SA_NONE;

	// a P or Esc pressed as the last game ended mustn't pause or open the menu in this one
	pause_pressed = false;
	ingamemenu_pressed = false;

	quick_start_players = net.players;
	quick_start_episode = net.episode;
	quick_start_difficulty = net.difficulty;
	quickStartArcade();
	quick_start_players = 0;

	fprintf(stderr, "net: game started: %u players, episode %u, difficulty %u, input delay %u, you are player %u\n",
	        net.players, net.episode, net.difficulty, net.input_delay, thisPlayerNum);
}

void net_end_game(void)
{
	if (!isNetworkGame)
		return;

	isNetworkGame = false;
	restore_settings();

	// nor in whatever is played next (the leave keys in wait_for_bundle() and hold() set them too)
	pause_pressed = false;
	ingamemenu_pressed = false;

	if (net.status == NET_IN_GAME)
	{
		net.status = NET_IN_LOBBY;
		const Uint8 data[1] = { NET_C_DONE };
		send_data(NET_CHANNEL_CONTROL, data, sizeof(data));
	}

	fprintf(stderr, "net: game over after %u ticks%s%s\n", (unsigned)net.tick,
	        net.end_message[0] != '\0' ? ": " : "", net.end_message);
}

/** Sends every local input the relay hasn't acknowledged (up to a packet's worth). */
static void send_inputs(void)
{
	const Uint32 first = net.input_acked + 1;
	Uint32 count = first <= net.queued_upto ? net.queued_upto - first + 1 : 0;
	if (count > NET_TICKS_PER_PACKET)
		count = NET_TICKS_PER_PACKET;

	Uint8 data[10 + NET_TICKS_PER_PACKET * (12 + NET_MAX_LOCAL * NET_INPUT_SIZE)];
	Uint8 *p = data;
	p = put_u8(p, NET_C_INPUT);
	p = put_u32(p, net.bundle_contig);
	p = put_u32(p, first);
	p = put_u8(p, count);
	for (Uint32 t = first; t < first + count; ++t)
	{
		const LocalEntry *entry = &net.local[t % NET_RING];
		p = put_u32(p, entry->hash_tick);
		p = put_u64(p, entry->hash);
		for (unsigned int i = 0; i < net.lobby.my_slot_count; ++i)
			p = put_input(p, &entry->input[i]);
	}

	send_data(NET_CHANNEL_TICKS, data, p - data);
}

static void queue_local_input(Uint32 tick, const PlayerInput *input)
{
	LocalEntry *entry = &net.local[tick % NET_RING];
	memset(entry, 0, sizeof(*entry));
	entry->hash_tick = net.last_hash_tick;
	entry->hash = net.last_hash;
	if (input != NULL)
		entry->input[0] = *input;
	net.queued_upto = tick;
}

static void draw_notice(int y, const char *text)
{
	SDL_Surface *screen = VGAScreenSeg;
	JE_barShade(screen, 3, y, 257, y + 20);
	JE_barShade(screen, 5, y + 2, 255, y + 18);
	JE_dString(screen, 130 - JE_textWidth(text, SMALL_FONT_SHAPES) / 2, y + 5, text, SMALL_FONT_SHAPES);
	JE_showVGA();
}

/** P or Enter ends a pause; not any key, or the other players' gameplay keys would. */
static bool resume_pressed(void)
{
	return newkey && (lastkey_scan == SDL_SCANCODE_P || lastkey_scan == SDL_SCANCODE_RETURN ||
	                  lastkey_scan == SDL_SCANCODE_KP_ENTER);
}

/** Waits for the bundle of `tick`, resending input meanwhile; false if the game ended or will stop first. */
static bool wait_for_bundle(Uint32 tick)
{
	const Uint32 start = SDL_GetTicks();
	Uint32 last_send = start;
	bool notice_shown = false;

	while (net.bundle_contig < tick)
	{
		if (net.ended || net.host == NULL)
			return false;
		if (net.stop_set && tick > net.stop_tick)
			return false;

		net_service(1);

		const Uint32 now = SDL_GetTicks();
		if (now - last_send >= NET_RESEND_MS)
		{
			send_inputs();
			last_send = now;
		}

		if (now - start >= NET_WAIT_NOTICE_MS)
		{
			if (!notice_shown)
			{
				draw_notice(60, "Waiting for other players. Esc leaves.");
				notice_shown = true;
			}

			service_SDL_events(true);
			if (newkey && lastkey_scan == SDL_SCANCODE_ESCAPE)
			{
				net_set_end_message("You left the game.");
				net_leave();
				return false;
			}
		}
	}

	return !net.ended;
}

static bool take_resume(Uint32 tick, Resume *out)
{
	for (unsigned int i = 0; i < net.resume_count; ++i)
	{
		if (net.resumes[i].tick == tick)
		{
			*out = net.resumes[i];
			memmove(&net.resumes[i], &net.resumes[i + 1], sizeof(net.resumes[0]) * (net.resume_count - i - 1));
			--net.resume_count;
			return true;
		}
	}
	return false;
}

/**
 * A pause or the in-game menu: every computer holds at the same tick until the relay says
 * to go on (or quit), so none of them simulates ahead.  `menu_slot` opened the menu, or is
 * NET_MAX_SLOTS for a pause.
 */
static void hold(Uint32 tick, unsigned int menu_slot)
{
	const bool pause = menu_slot >= NET_MAX_SLOTS;
	const bool my_menu = !pause && is_local_slot(menu_slot);
	bool resume_sent = false;

	if (pause)
		fprintf(stderr, "net: paused at tick %u\n", (unsigned)tick);
	else
		fprintf(stderr, "net: %s opened the menu at tick %u\n", net_player_name_of(menu_slot + 1), (unsigned)tick);

	if (my_menu)
	{
		yourInGameMenuRequest = true;
		JE_doInGameSetup();

		const bool quit = reallyEndLevel && playerEndLevel;
		if (!net.ended)
		{
			reallyEndLevel = false;  // not yet: everyone quits together when the relay says so
			playerEndLevel = false;
		}

		const Uint8 data[6] = { NET_C_RESUME, tick & 0xff, (tick >> 8) & 0xff, (tick >> 16) & 0xff, tick >> 24, quit };
		send_data(NET_CHANNEL_CONTROL, data, sizeof(data));
		resume_sent = true;
	}
	else if (pause)
	{
		mouseSetRelative(false);
		JE_dString(VGAScreenSeg, 120, 90, miscText[22], FONT_SHAPES);
		draw_notice(120, "P or Enter goes on");
	}
	else
	{
		char text[64];
		snprintf(text, sizeof(text), "%s is in the menu. Esc leaves.", net_player_name_of(menu_slot + 1));
		draw_notice(60, text);
	}

	service_SDL_events(true);

	Resume resume;
	bool resumed = false;
	while (!(resumed = take_resume(tick, &resume)))
	{
		// Ended, or the relay stopped the game (a player left, a desync): nobody will let go
		// of this hold, so carry on to the stop tick like everyone else.
		if (net.ended || net.host == NULL || net.stop_set)
			break;

		net_service(10);
		service_SDL_events(true);

		// anyone can end a pause
		if (pause && !resume_sent && resume_pressed())
		{
			const Uint8 data[6] = { NET_C_RESUME, tick & 0xff, (tick >> 8) & 0xff, (tick >> 16) & 0xff, tick >> 24, 0 };
			send_data(NET_CHANNEL_CONTROL, data, sizeof(data));
			resume_sent = true;
		}

		// someone else's menu can stay open; Esc leaves the game instead of waiting
		if (!pause && !my_menu && newkey && lastkey_scan == SDL_SCANCODE_ESCAPE)
		{
			net_set_end_message("You left the game.");
			net_leave();
			break;
		}
	}

	if (pause)
		mouseSetRelative(true);

	if (!resumed)
		return;

	fprintf(stderr, "net: %s %s at tick %u\n", net_player_name_of(resume.slot + 1),
	        resume.quit ? "ended the game" : "went on", (unsigned)tick);

	if (resume.quit)
	{
		char text[96];
		if (is_local_slot(resume.slot))
			snprintf(text, sizeof(text), "You ended the game.");
		else
			snprintf(text, sizeof(text), "%s ended the game.", net_player_name_of(resume.slot + 1));
		end_now(text);
	}
}

/** The relay stopped the game and this computer has simulated every tick it allowed. */
static void stop_reached(void)
{
	char text[128];

	if (net.stop_reason == NET_END_DESYNC)
	{
		// every computer dumps the same tick; the player number keeps two on one computer apart
		char name[64], path[256];
		snprintf(name, sizeof(name), "desync-tick-%u-player%u.txt", (unsigned)net.tick, thisPlayerNum);
		snprintf(path, sizeof(path), "%s/%s", get_user_directory(), name);
		sync_write_state_dump(path);
		snprintf(text, sizeof(text), "Out of sync at tick %u! Saved %s", (unsigned)net.desync_tick, name);
		fprintf(stderr, "net: DESYNC at tick %u; state at tick %u written to %s\n",
		        (unsigned)net.desync_tick, (unsigned)net.tick, path);
	}
	else
	{
		const char *name = net_player_name_of(net.stop_slot + 1);
		snprintf(text, sizeof(text), "%s left the game.", name[0] != '\0' ? name : "A player");
	}

	end_now(text);
}

void net_tick_begin(void)
{
	if (!isNetworkGame)
		return;

	// Once ended, keep asking: a level start, a respawn or the menu clears these flags, and
	// the level would otherwise go on with nobody's input.
	if (net.ended)
	{
		reallyEndLevel = true;
		playerEndLevel = true;
		return;
	}

	const Uint32 tick = net.tick + 1;

	if (net.stop_set && tick > net.stop_tick)
	{
		stop_reached();
		return;
	}

	// the first ticks have no input from anyone: the input delay hasn't passed yet
	if (tick == 1)
		for (Uint32 t = 1; t <= net.input_delay; ++t)
			queue_local_input(t, NULL);

	PlayerInput input;
	read_local_input(&input, 0);
	if (pause_pressed)
		input.buttons |= PLAYER_INPUT_PAUSE;
	if (ingamemenu_pressed)
		input.buttons |= PLAYER_INPUT_MENU;
	pause_pressed = false;
	ingamemenu_pressed = false;

	queue_local_input(tick + net.input_delay, &input);
	send_inputs();

	if (!wait_for_bundle(tick))
	{
		if (net.stop_set && tick > net.stop_tick && !net.ended)
			stop_reached();
		end_now(net.error[0] != '\0' ? net.error : NULL);
		return;
	}

	net.tick = tick;

	unsigned int menu_slot = NET_MAX_SLOTS;
	bool pause = false;
	for (unsigned int s = 0; s < net.players; ++s)
	{
		const Uint8 buttons = net.bundles[tick % NET_RING][s].buttons;
		if ((buttons & PLAYER_INPUT_MENU) && menu_slot == NET_MAX_SLOTS)
			menu_slot = s;
		if (buttons & PLAYER_INPUT_PAUSE)
			pause = true;
	}
	if (menu_slot < NET_MAX_SLOTS || pause)
		hold(tick, menu_slot);
}

void net_tick_end(void)
{
	if (!isNetworkGame || net.ended)
		return;

	// Test hook: OPENTYRIAN_TEST_DESYNC_TICK=N changes this computer's game at tick N, so
	// tests/net/run-local.ps1 -Desync can check that desyncs are caught and reported.
	static long desync_test_tick = -1;
	if (desync_test_tick < 0)
	{
		const char *value = getenv("OPENTYRIAN_TEST_DESYNC_TICK");
		desync_test_tick = value != NULL ? strtol(value, NULL, 10) : 0;
	}
	if (desync_test_tick > 0 && net.tick == (Uint32)desync_test_tick)
		player[0].cash += 1;

	net.last_hash = sync_state_hash();
	net.last_hash_tick = net.tick;
}

void net_get_input(unsigned int player_num, PlayerInput *input)
{
	memset(input, 0, sizeof(*input));
	if (net.ended || net.tick == 0 || net.tick > net.bundle_contig || player_num < 1 || player_num > net.players)
		return;

	*input = net.bundles[net.tick % NET_RING][player_num - 1];
	input->buttons &= PLAYER_INPUT_GAME_BUTTONS;
}

/* --- LAN discovery --- */

void net_discovery_start(void)
{
	net_discovery_stop();
	net.lan_count = 0;

	if (!ensure_enet())
		return;

	net.discovery = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	if (net.discovery == ENET_SOCKET_NULL)
		return;

	enet_socket_set_option(net.discovery, ENET_SOCKOPT_NONBLOCK, 1);
	enet_socket_set_option(net.discovery, ENET_SOCKOPT_BROADCAST, 1);
	ENetAddress any = { .host = ENET_HOST_ANY, .port = 0 };
	enet_socket_bind(net.discovery, &any);

	net.discovering = true;
	net.discovery_last_query = SDL_GetTicks() - 10000;
}

static void send_discovery_query(enet_uint32 host)
{
	Uint8 query[NET_DISCOVERY_QUERY_SIZE];
	Uint8 *p = put_bytes(query, "TYR?", 4);
	put_u16(p, NET_PROTOCOL_VERSION);

	ENetAddress to = { .host = host, .port = NET_DISCOVERY_PORT };
	ENetBuffer buffer = { .data = query, .dataLength = sizeof(query) };
	enet_socket_send(net.discovery, &to, &buffer, 1);
}

unsigned int net_discovery_poll(NetLanGame *games, unsigned int max)
{
	if (!net.discovering)
		return 0;

	const Uint32 now = SDL_GetTicks();

	if (now - net.discovery_last_query >= 1000)
	{
		net.discovery_last_query = now;
		send_discovery_query(ENET_HOST_BROADCAST);

		ENetAddress loopback;
		enet_address_set_host_ip(&loopback, "127.0.0.1");  // a game hosted on this computer
		send_discovery_query(loopback.host);
	}

	for (int n = 0; n < 32; ++n)
	{
		Uint8 answer[128];
		ENetAddress from;
		ENetBuffer buffer = { .data = answer, .dataLength = sizeof(answer) };
		const int length = enet_socket_receive(net.discovery, &from, &buffer, 1);
		if (length <= 0)
			break;

		Reader r = { answer, answer + length, true };
		const Uint8 *magic = get_bytes(&r, 4);
		const Uint16 version = get_u16(&r);
		NetLanGame game;
		memset(&game, 0, sizeof(game));
		game.port = get_u16(&r);
		const Uint8 *code = get_bytes(&r, NET_CODE_LEN);
		if (code != NULL)
			memcpy(game.code, code, NET_CODE_LEN);
		get_string(&r, game.host_name, NET_NAME_LEN);
		game.players = get_u8(&r);
		game.max_players = get_u8(&r);
		game.in_game = get_u8(&r) != 0;
		if (!r.ok || memcmp(magic, "TYR!", 4) != 0 || version != NET_PROTOCOL_VERSION)
			continue;
		enet_address_get_host_ip(&from, game.address, sizeof(game.address));

		// the same game answers on every address it's reachable at; list it once
		unsigned int i = 0;
		while (i < net.lan_count && strcmp(net.lan[i].game.code, game.code) != 0)
			++i;
		if (i == net.lan_count)
		{
			if (net.lan_count == NET_MAX_LAN_GAMES)
				continue;
			++net.lan_count;
		}
		else if (strcmp(net.lan[i].game.address, "127.0.0.1") != 0)
		{
			SDL_strlcpy(game.address, net.lan[i].game.address, sizeof(game.address));  // keep the LAN address
		}
		net.lan[i].game = game;
		net.lan[i].seen = now;
	}

	unsigned int count = 0;
	for (unsigned int i = 0; i < net.lan_count; )
	{
		if (now - net.lan[i].seen > NET_LAN_GAME_EXPIRY)
		{
			net.lan[i] = net.lan[--net.lan_count];
			continue;
		}
		if (count < max)
			games[count++] = net.lan[i].game;
		++i;
	}
	return count;
}

void net_discovery_stop(void)
{
	if (net.discovering && net.discovery != ENET_SOCKET_NULL)
		enet_socket_destroy(net.discovery);
	net.discovery = ENET_SOCKET_NULL;
	net.discovering = false;
}

/* --- settings --- */

const char *net_current_name(void)
{
	return net_name_option != NULL ? net_name_option : net_player_name;
}

const char *net_current_relay(void)
{
	return net_relay_option != NULL ? net_relay_option : net_relay_address;
}

void net_set_default_config(void)
{
	// the computer's user name, as far as the game's fonts can show it
	const char *user = getenv("USERNAME");
	if (user == NULL)
		user = getenv("USER");

	size_t n = 0;
	for (; user != NULL && *user != '\0' && n < 12; ++user)
	{
		const unsigned char ch = (unsigned char)*user;
		if (isalnum(ch) || ch == ' ' || ch == '-' || ch == '_')
			net_player_name[n++] = (char)toupper(ch);
	}
	net_player_name[n] = '\0';
	if (n == 0)
		strcpy(net_player_name, "PLAYER");

	SDL_strlcpy(net_relay_address, NET_DEFAULT_RELAY, sizeof(net_relay_address));
}
