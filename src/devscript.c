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

bool devscript_load(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL)
	{
		fprintf(stderr, "error: failed to open dev script '%s'\n", path);
		return false;
	}

	lines = malloc(DEVSCRIPT_MAX_LINES * sizeof(*lines));
	line_count = 0;

	char buffer[DEVSCRIPT_LINE_LEN];
	while (line_count < DEVSCRIPT_MAX_LINES && fgets(buffer, sizeof(buffer), f) != NULL)
	{
		buffer[strcspn(buffer, "\r\n")] = '\0';

		const char *start = buffer + strspn(buffer, " \t");
		if (*start == '\0' || *start == '#')
			continue;

		SDL_strlcpy(lines[line_count++], start, DEVSCRIPT_LINE_LEN);
	}

	fclose(f);

	devscript_enabled = true;

	// Scripted runs usually happen in the background; don't let the game auto-pause.
	windowHasFocus = true;

	return true;
}

static void push_key_event(Uint32 type, SDL_Scancode scancode)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));

	ev.type = type;
	ev.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
	ev.key.windowID = SDL_GetWindowID(main_window);
	ev.key.keysym.scancode = scancode;
	ev.key.keysym.sym = SDL_GetKeyFromScancode(scancode);

	SDL_PushEvent(&ev);
}

static void run_line(const char *line, Uint32 now)
{
	char command[32] = "", arg1[DEVSCRIPT_LINE_LEN] = "", arg2[32] = "";
	sscanf(line, "%31s %255s %31s", command, arg1, arg2);

	if (strcmp(command, "wait") == 0)
	{
		resume_tick = now + atoi(arg1);
	}
	else if (strcmp(command, "key") == 0)
	{
		SDL_Scancode scancode = SDL_GetScancodeFromName(arg1);
		if (scancode == SDL_SCANCODE_UNKNOWN)
		{
			fprintf(stderr, "devscript: unknown key '%s'\n", arg1);
			return;
		}

		push_key_event(SDL_KEYDOWN, scancode);

		held_key = scancode;
		release_tick = now + (arg2[0] != '\0' ? atoi(arg2) : 100);
		resume_tick = release_tick;
	}
	else if (strcmp(command, "shot") == 0)
	{
		SDL_strlcpy(shot_path, arg1, sizeof(shot_path));
		shot_pending = true;
		shot_tick = now;
	}
	else if (strcmp(command, "window") == 0)
	{
		SDL_SetWindowSize(main_window, atoi(arg1), atoi(arg2));
	}
	else if (strcmp(command, "mode") == 0)
	{
		// mode names can contain spaces, so take the rest of the line
		const char *name = line + strlen(command);
		name += strspn(name, " \t");
		if (!set_scaling_mode_by_name(name))
			fprintf(stderr, "devscript: unknown scaling mode '%s'\n", name);
	}
	else if (strcmp(command, "quit") == 0)
	{
		JE_tyrianHalt(0);
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
