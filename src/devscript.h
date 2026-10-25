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
#ifndef DEVSCRIPT_H
#define DEVSCRIPT_H

#include "opentyr.h"

#include "SDL.h"

/*
 * Development test scripts (--dev-script=FILE).  A script drives the game with
 * synthetic input and captures the final window output, so changes can be
 * checked without anyone at the keyboard.  One command per line:
 *
 *   wait MS               pause the script for MS milliseconds
 *   key NAME [HOLD_MS]    press and release a key (SDL scancode name, e.g. Return;
 *                         write spaces as underscores, e.g. Left_Ctrl)
 *   down NAME / up NAME   press or release a key, for holding several at once
 *   shot PATH             save the next presented frame as a BMP
 *   window W H            resize the window
 *   mode NAME             set the scaling mode (e.g. Widescreen)
 *   quit [CODE]           exit the game, with exit code CODE (default 0)
 *
 * Lines starting with '#' are comments.
 */

extern bool devscript_enabled;

bool devscript_load(const char *path);

void devscript_tick(void);
void devscript_on_present(SDL_Renderer *renderer);

#endif /* DEVSCRIPT_H */
