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
#include "netmenu.h"

#include "config.h"
#include "episodes.h"
#include "font.h"
#include "joystick.h"
#include "keyboard.h"
#include "menus.h"
#include "netplay.h"
#include "nortsong.h"
#include "palette.h"
#include "picload.h"
#include "sndmast.h"
#include "video.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define NAME_MAX_CHARS 12

static bool first_frame;

/** Starts a screen: the menu background, faded in when it is first shown. */
static void begin_screen(void)
{
	JE_loadPic(VGAScreen2, 2, false);
	first_frame = true;
}

static void draw_background(const char *title)
{
	memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);
	draw_font_hv_shadow(VGAScreen, 320 / 2, 20, title, large_font, centered, 15, -3, false, 2);
}

static void draw_item(int y, const char *text, bool selected, bool dim)
{
	draw_font_hv_shadow(VGAScreen, 320 / 2, y, text, normal_font, centered, 15, -4 + (selected ? 2 : 0) + (dim ? -4 : 0), false, 2);
}

static void draw_note(int y, const char *text, bool bright)
{
	draw_font_hv_shadow(VGAScreen, 320 / 2, y, text, small_font, centered, 15, bright ? 2 : -3, false, 1);
}

static void present(void)
{
	JE_showVGA();
	if (first_frame)
	{
		fade_palette(colors, 10, 0, 255);
		first_frame = false;
	}
}

/** Waits a frame and reads input; menus here redraw every frame, as the network can change what they show. */
static void next_frame(void)
{
	SDL_Delay(16);
	push_joysticks_as_keyboard();
	service_SDL_events(true);
}

/** A list of choices.  Returns the one chosen, or -1 if the player backed out. */
static int menu(const char *title, const char *const *items, int count, int *selected, const char *note)
{
	for (; ; )
	{
		draw_background(title);
		for (int i = 0; i < count; ++i)
			draw_item(54 + 24 * i, items[i], i == *selected, false);
		if (note != NULL && note[0] != '\0')
			draw_note(180, note, true);
		present();

		next_frame();

		if (!newkey)
			continue;

		switch (lastkey_scan)
		{
		case SDL_SCANCODE_UP:
			JE_playSampleNum(S_CURSOR);
			*selected = *selected == 0 ? count - 1 : *selected - 1;
			break;
		case SDL_SCANCODE_DOWN:
			JE_playSampleNum(S_CURSOR);
			*selected = *selected == count - 1 ? 0 : *selected + 1;
			break;
		case SDL_SCANCODE_SPACE:
		case SDL_SCANCODE_RETURN:
			JE_playSampleNum(S_SELECT);
			return *selected;
		case SDL_SCANCODE_ESCAPE:
			JE_playSampleNum(S_SPRING);
			return -1;
		default:
			break;
		}
	}
}

/** Typing a room code (exactly NET_CODE_LEN letters and digits) or a name. */
static bool text_entry(const char *title, const char *prompt, char *buffer, size_t size, bool code)
{
	const size_t max_chars = code ? NET_CODE_LEN : NAME_MAX_CHARS;
	char text[NET_NAME_LEN] = "";
	SDL_strlcpy(text, buffer, SDL_min(sizeof(text), max_chars + 1));

	for (Uint32 frame = 0; ; ++frame)
	{
		draw_background(title);
		draw_item(70, prompt, false, false);

		char shown[NET_NAME_LEN + 2];
		snprintf(shown, sizeof(shown), "%s%s", text, (frame / 16) % 2 == 0 && strlen(text) < max_chars ? "_" : " ");
		draw_item(100, shown, true, false);

		draw_note(172, "Enter accepts, Esc goes back", false);
		present();

		next_frame();

		if (new_text)
		{
			for (const char *c = last_text; *c != '\0'; ++c)
			{
				const unsigned char ch = (unsigned char)toupper((unsigned char)*c);
				const bool allowed = ch < 128 && (isalnum(ch) || (!code && (ch == ' ' || ch == '-')));
				const size_t length = strlen(text);
				if (allowed && length < max_chars)
				{
					text[length] = (char)ch;
					text[length + 1] = '\0';
				}
			}
		}

		if (!newkey)
			continue;

		switch (lastkey_scan)
		{
		case SDL_SCANCODE_BACKSPACE:
			if (text[0] != '\0')
				text[strlen(text) - 1] = '\0';
			break;
		case SDL_SCANCODE_RETURN:
		case SDL_SCANCODE_KP_ENTER:
			if (code ? strlen(text) != NET_CODE_LEN : text[0] == '\0')
			{
				JE_playSampleNum(S_CLINK);
				break;
			}
			JE_playSampleNum(S_SELECT);
			SDL_strlcpy(buffer, text, size);
			return true;
		case SDL_SCANCODE_ESCAPE:
			JE_playSampleNum(S_SPRING);
			return false;
		default:
			break;
		}
	}
}

/** Lists the games hosted on the LAN; connects to the one chosen. */
static bool lan_list(void)
{
	net_discovery_start();

	unsigned int selected = 0;
	for (; ; )
	{
		NetLanGame games[NET_MAX_LAN_GAMES];
		const unsigned int count = net_discovery_poll(games, COUNTOF(games));
		if (selected >= count)
			selected = count > 0 ? count - 1 : 0;

		draw_background("Join LAN Game");
		if (count == 0)
			draw_item(90, "Looking for games...", false, true);
		for (unsigned int i = 0; i < count; ++i)
		{
			char text[64];
			snprintf(text, sizeof(text), "%s  %u/%u%s", games[i].host_name, games[i].players, games[i].max_players,
			         games[i].in_game ? "  playing" : "");
			const bool full = games[i].in_game || games[i].players >= games[i].max_players;
			draw_item(54 + 20 * i, text, i == selected, full);
		}
		draw_note(172, "Enter joins, Esc goes back", false);
		present();

		next_frame();

		if (!newkey)
			continue;

		switch (lastkey_scan)
		{
		case SDL_SCANCODE_UP:
			if (count > 0)
			{
				JE_playSampleNum(S_CURSOR);
				selected = selected == 0 ? count - 1 : selected - 1;
			}
			break;
		case SDL_SCANCODE_DOWN:
			if (count > 0)
			{
				JE_playSampleNum(S_CURSOR);
				selected = selected == count - 1 ? 0 : selected + 1;
			}
			break;
		case SDL_SCANCODE_SPACE:
		case SDL_SCANCODE_RETURN:
		{
			if (count == 0 || games[selected].in_game || games[selected].players >= games[selected].max_players)
			{
				JE_playSampleNum(S_CLINK);
				break;
			}
			JE_playSampleNum(S_SELECT);
			net_discovery_stop();

			char address[80];
			snprintf(address, sizeof(address), "%s:%u", games[selected].address, (unsigned)games[selected].port);
			return net_connect(address, games[selected].port, games[selected].code);
		}
		case SDL_SCANCODE_ESCAPE:
			JE_playSampleNum(S_SPRING);
			net_discovery_stop();
			return false;
		default:
			break;
		}
	}
}

bool netmenu_run(void)
{
	static int selected = 0;
	const char *note = NULL;

	begin_screen();

	for (; ; )
	{
		char name_item[40];
		snprintf(name_item, sizeof(name_item), "Your Name: %s", net_current_name());
		const char *const items[] = { "Host Internet Game", "Join Internet Game", "Host LAN Game", "Join LAN Game", name_item };

		const int choice = menu("Network Game", items, COUNTOF(items), &selected, note);
		note = NULL;

		bool connected = false;
		switch (choice)
		{
		case -1:
			fade_black(10);
			return false;

		case 0:
			connected = net_connect(net_current_relay(), NET_DEFAULT_PORT, NULL);
			break;

		case 1:
		{
			char code[NET_CODE_LEN + 1] = "";
			if (!text_entry("Join Internet Game", "Type the room code:", code, sizeof(code), true))
				continue;
			connected = net_connect(net_current_relay(), NET_DEFAULT_PORT, code);
			break;
		}

		case 2:
			connected = net_host_here();
			break;

		case 3:
			connected = lan_list();
			break;

		case 4:
		{
			char name[NET_NAME_LEN];
			SDL_strlcpy(name, net_current_name(), sizeof(name));
			if (text_entry("Your Name", "Type your name:", name, sizeof(name), false))
			{
				SDL_strlcpy(net_player_name, name, sizeof(net_player_name));
				net_name_option = NULL;
			}
			continue;
		}
		}

		if (connected && netmenu_lobby())
			return true;

		note = net_error();
		begin_screen();
	}
}

void netmenu_message(const char *text)
{
	begin_screen();

	for (; ; )
	{
		draw_background("Network Game");
		draw_note(90, text[0] != '\0' ? text : "The network game ended.", true);
		draw_note(172, "Press a key", false);
		present();

		next_frame();

		if (newkey || newmouse)
			break;
	}

	fade_black(10);
}

static unsigned int next_episode(unsigned int episode, int direction)
{
	for (int i = 0; i < EPISODE_AVAILABLE; ++i)
	{
		episode = (episode - 1 + EPISODE_AVAILABLE + direction) % EPISODE_AVAILABLE + 1;
		if (episodeAvail[episode - 1])
			return episode;
	}
	return 1;
}

bool netmenu_lobby(void)
{
	// the host's choices, kept for the next game
	static unsigned int episode = 1, difficulty = DIFFICULTY_NORMAL;
	unsigned int row = 0;  // the setting the host is changing: episode or difficulty
	bool settings_sent = false;

	if (!episodeAvail[episode - 1])
		episode = next_episode(episode, 1);

	begin_screen();

	for (; ; )
	{
		const NetStatus status = net_status();

		if (status == NET_OFFLINE)
		{
			fade_black(10);
			return false;
		}
		if (net_start_received())
		{
			JE_playSampleNum(S_SELECT);
			fade_black(10);
			return true;
		}

		const NetLobby *lobby = net_lobby();
		const bool host = net_is_host();
		const unsigned int players = net_lobby_players();

		// whoever is host (it moves on if the host leaves) tells the room its choices
		if (status == NET_IN_LOBBY && host && !settings_sent)
		{
			net_send_settings(episode, difficulty);
			settings_sent = true;
		}
		if (!host)
			settings_sent = false;

		draw_background("Network Game");

		if (status == NET_CONNECTING)
		{
			draw_item(90, "Connecting...", false, false);
			draw_note(172, "Esc cancels", false);
		}
		else
		{
			char text[96];

			if (net_hosting_here())
				snprintf(text, sizeof(text), "LAN game on %s", net_local_address()[0] != '\0' ? net_local_address() : "this computer");
			else
				snprintf(text, sizeof(text), "Room code: %s", net_room_code());
			draw_item(46, text, true, false);

			// (the menu font has no brackets, so who's who goes on a line of its own)
			for (unsigned int i = 0; i < lobby->max_players && i < NET_MAX_SLOTS; ++i)
			{
				if (lobby->used[i])
					snprintf(text, sizeof(text), "Player %u: %s", i + 1, lobby->names[i]);
				else
					snprintf(text, sizeof(text), "Player %u: waiting...", i + 1);
				draw_item(66 + 14 * i, text, false, !lobby->used[i]);
			}
			if (host)
				snprintf(text, sizeof(text), "You are player %u, and the host.", lobby->my_slot + 1);
			else
				snprintf(text, sizeof(text), "You are player %u. %s is the host.", lobby->my_slot + 1, lobby->names[lobby->host_slot % NET_MAX_SLOTS]);
			draw_note(66 + 14 * lobby->max_players + 2, text, false);

			const unsigned int shown_episode = host ? episode : lobby->episode;
			const unsigned int shown_difficulty = host ? difficulty : lobby->difficulty;
			const char *episode_text = shown_episode >= 1 && shown_episode <= EPISODE_AVAILABLE ? episode_name[shown_episode] : "?";
			const char *difficulty_text = shown_difficulty >= 1 && shown_difficulty < COUNTOF(difficulty_name) ? difficulty_name[shown_difficulty] : "?";

			snprintf(text, sizeof(text), "%s", episode_text);
			draw_item(110, text, host && row == 0, false);
			snprintf(text, sizeof(text), "Difficulty: %s", difficulty_text);
			draw_item(126, text, host && row == 1, false);

			if (lobby->in_game)
				draw_note(152, "Waiting for everyone to finish the last game...", false);
			else if (host && players < 2)
				draw_note(152, "Waiting for someone to join...", false);
			else if (host)
				draw_note(152, "Press Enter to start!", true);
			else
				draw_note(152, "Waiting for the host to start...", false);

			draw_note(164, host ? "Arrows change the game, Esc leaves" : "Esc leaves", false);

			if (net_end_message()[0] != '\0')
				draw_note(180, net_end_message(), true);
		}

		present();
		next_frame();

		if (!newkey)
			continue;

		if (lastkey_scan == SDL_SCANCODE_ESCAPE)
		{
			JE_playSampleNum(S_SPRING);
			net_leave();
			fade_black(10);
			return false;
		}

		if (status != NET_IN_LOBBY || !host)
			continue;

		switch (lastkey_scan)
		{
		case SDL_SCANCODE_UP:
		case SDL_SCANCODE_DOWN:
			JE_playSampleNum(S_CURSOR);
			row = 1 - row;
			break;
		case SDL_SCANCODE_LEFT:
		case SDL_SCANCODE_RIGHT:
		{
			const int direction = lastkey_scan == SDL_SCANCODE_LEFT ? -1 : 1;
			JE_playSampleNum(S_CURSOR);
			if (row == 0)
				episode = next_episode(episode, direction);
			else
				difficulty = (difficulty - DIFFICULTY_EASY + 3 + direction) % 3 + DIFFICULTY_EASY;  // easy, normal, hard
			net_send_settings(episode, difficulty);
			break;
		}
		case SDL_SCANCODE_SPACE:
		case SDL_SCANCODE_RETURN:
			if (lobby->in_game || players < 2)
			{
				JE_playSampleNum(S_CLINK);
				break;
			}
			net_send_settings(episode, difficulty);
			net_send_start();
			break;
		default:
			break;
		}
	}
}
