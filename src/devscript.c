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
#include "devscript.h"

#include "joystick.h"
#include "keyboard.h"
#include "varz.h"
#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVSCRIPT_MAX_LINES 1024
#define DEVSCRIPT_LINE_LEN  256

bool devscript_enabled = false;

static char (*lines)[DEVSCRIPT_LINE_LEN] = NULL;
static int line_count = 0, next_line = 0;

static Uint32 resume_tick = 0;

static SDL_Scancode held_key = SDL_SCANCODE_UNKNOWN;
static Uint32 release_tick = 0;

static char shot_path[DEVSCRIPT_LINE_LEN];
static bool shot_pending = false;
static Uint32 shot_tick = 0;

/** Removes trailing whitespace in place. */
static void trim_end(char *s)
{
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
		s[--len] = '\0';
}

bool devscript_load(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL)
	{
		fprintf(stderr, "error: failed to open dev script '%s'\n", path);
		return false;
	}

	lines = malloc(DEVSCRIPT_MAX_LINES * sizeof(*lines));
	if (lines == NULL)
	{
		fclose(f);
		return false;
	}
	line_count = 0;

	char buffer[DEVSCRIPT_LINE_LEN];
	for (int line_number = 1; fgets(buffer, sizeof(buffer), f) != NULL; ++line_number)
	{
		const size_t len = strcspn(buffer, "\r\n");
		if (buffer[len] == '\0' && !feof(f))
		{
			fprintf(stderr, "error: dev script line %d is longer than %d characters\n", line_number, DEVSCRIPT_LINE_LEN - 2);
			fclose(f);
			return false;
		}
		buffer[len] = '\0';
		trim_end(buffer);

		const char *start = buffer + strspn(buffer, " \t");
		if (*start == '\0' || *start == '#')
			continue;

		if (line_count == DEVSCRIPT_MAX_LINES)
		{
			fprintf(stderr, "error: dev script has more than %d commands\n", DEVSCRIPT_MAX_LINES);
			fclose(f);
			return false;
		}
		SDL_strlcpy(lines[line_count++], start, DEVSCRIPT_LINE_LEN);
	}

	fclose(f);

	devscript_enabled = true;

	// Scripted runs usually happen in the background; don't let the game auto-pause.
	windowHasFocus = true;

	// Only scripted input may reach the game; a gamepad plugged in would add its own.
	ignore_joystick = true;

	return true;
}

static void push_key_event(Uint32 type, SDL_Scancode scancode)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));

	ev.type = type;
	ev.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
	ev.key.windowID = DEVSCRIPT_WINDOW_ID;
	ev.key.keysym.scancode = scancode;
	ev.key.keysym.sym = SDL_GetKeyFromScancode(scancode);

	SDL_PushEvent(&ev);
}

static void push_mouse_motion(int dx, int dy)
{
	int w, h;
	SDL_GetWindowSize(main_window, &w, &h);

	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));

	ev.type = SDL_MOUSEMOTION;
	ev.motion.windowID = DEVSCRIPT_WINDOW_ID;
	ev.motion.x = w / 2;
	ev.motion.y = h / 2;
	ev.motion.xrel = dx;
	ev.motion.yrel = dy;

	SDL_PushEvent(&ev);
}

static void push_mouse_button(Uint32 type, Uint8 button)
{
	int w, h;
	SDL_GetWindowSize(main_window, &w, &h);

	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));

	ev.type = type;
	ev.button.windowID = DEVSCRIPT_WINDOW_ID;
	ev.button.button = button;
	ev.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
	ev.button.clicks = 1;
	ev.button.x = w / 2;
	ev.button.y = h / 2;

	SDL_PushEvent(&ev);
}

/** The text after the command word, for arguments that may contain spaces. */
static const char *rest_of_line(const char *line)
{
	const char *rest = line + strcspn(line, " \t");
	return rest + strspn(rest, " \t");
}

static SDL_Scancode parse_key(char *name)
{
	// key names with spaces are written with underscores, e.g. Left_Ctrl
	for (char *c = name; *c != '\0'; ++c)
		if (*c == '_')
			*c = ' ';

	SDL_Scancode scancode = SDL_GetScancodeFromName(name);
	if (scancode == SDL_SCANCODE_UNKNOWN)
		fprintf(stderr, "devscript: unknown key '%s'\n", name);
	return scancode;
}

/** Parses a non-negative millisecond count, treating anything else as 0. */
static Uint32 parse_ms(const char *text)
{
	const long ms = strtol(text, NULL, 10);
	if (ms < 0)
	{
		fprintf(stderr, "devscript: negative time '%s' treated as 0\n", text);
		return 0;
	}
	return ms;
}

static void run_line(const char *line, Uint32 now)
{
	char command[32] = "", arg1[DEVSCRIPT_LINE_LEN] = "", arg2[32] = "";
	sscanf(line, "%31s %255s %31s", command, arg1, arg2);

	if (strcmp(command, "wait") == 0)
	{
		resume_tick = now + parse_ms(arg1);
	}
	else if (strcmp(command, "key") == 0)
	{
		SDL_Scancode scancode = parse_key(arg1);
		if (scancode == SDL_SCANCODE_UNKNOWN)
			return;

		push_key_event(SDL_KEYDOWN, scancode);

		held_key = scancode;
		release_tick = now + (arg2[0] != '\0' ? parse_ms(arg2) : 100);
		resume_tick = release_tick;
	}
	else if (strcmp(command, "down") == 0 || strcmp(command, "up") == 0)
	{
		SDL_Scancode scancode = parse_key(arg1);
		if (scancode == SDL_SCANCODE_UNKNOWN)
			return;

		push_key_event(command[0] == 'd' ? SDL_KEYDOWN : SDL_KEYUP, scancode);
	}
	else if (strcmp(command, "mouse") == 0)
	{
		push_mouse_motion(atoi(arg1), atoi(arg2));
	}
	else if (strcmp(command, "mousedown") == 0 || strcmp(command, "mouseup") == 0)
	{
		const int button = atoi(arg1);
		if (button < 1 || button > 3)
		{
			fprintf(stderr, "devscript: mouse button must be 1, 2 or 3, not '%s'\n", arg1);
			return;
		}
		push_mouse_button(command[5] == 'd' ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP, button);
	}
	else if (strcmp(command, "shot") == 0)
	{
		SDL_strlcpy(shot_path, rest_of_line(line), sizeof(shot_path));
		shot_pending = true;
		shot_tick = now;
	}
	else if (strcmp(command, "window") == 0)
	{
		SDL_SetWindowSize(main_window, atoi(arg1), atoi(arg2));
	}
	else if (strcmp(command, "mode") == 0)
	{
		const char *name = rest_of_line(line);
		if (!set_scaling_mode_by_name(name))
			fprintf(stderr, "devscript: unknown scaling mode '%s'\n", name);
	}
	else if (strcmp(command, "quit") == 0)
	{
		const int code = atoi(arg1);
		if (code == 0)
			JE_tyrianHalt(0);

		// JE_tyrianHalt treats some codes specially and truncates to a byte, so exit directly
		fprintf(stderr, "devscript: quit with exit code %d\n", code);
		exit(code);
	}
	else
	{
		fprintf(stderr, "devscript: unknown command '%s'\n", command);
	}
}

void devscript_tick(void)
{
	if (!devscript_enabled)
		return;

	const Uint32 now = SDL_GetTicks();

	if (held_key != SDL_SCANCODE_UNKNOWN && now >= release_tick)
	{
		push_key_event(SDL_KEYUP, held_key);
		held_key = SDL_SCANCODE_UNKNOWN;
	}

	// Menus only redraw when something changes; if no frame arrives soon, present one.
	if (shot_pending && now - shot_tick > 250)
		JE_showVGA();

	while (!shot_pending && held_key == SDL_SCANCODE_UNKNOWN &&
	       next_line < line_count && now >= resume_tick)
	{
		run_line(lines[next_line++], now);
	}
}

bool devscript_allows_event(const SDL_Event *ev)
{
	if (!devscript_enabled)
		return true;

	switch (ev->type)
	{
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		return ev->key.windowID == DEVSCRIPT_WINDOW_ID;
	case SDL_MOUSEMOTION:
		return ev->motion.windowID == DEVSCRIPT_WINDOW_ID;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		return ev->button.windowID == DEVSCRIPT_WINDOW_ID;
	case SDL_MOUSEWHEEL:
	case SDL_TEXTINPUT:
		return false;
	default:
		return true;
	}
}

void devscript_on_present(SDL_Renderer *renderer)
{
	if (!shot_pending)
		return;

	shot_pending = false;

	int w, h;
	SDL_GetRendererOutputSize(renderer, &w, &h);

	SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (surface == NULL)
		return;

	if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) != 0 ||
	    SDL_SaveBMP(surface, shot_path) != 0)
	{
		fprintf(stderr, "devscript: failed to save '%s': %s\n", shot_path, SDL_GetError());
	}

	SDL_FreeSurface(surface);
}
