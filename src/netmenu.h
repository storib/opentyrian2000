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
#ifndef NETMENU_H
#define NETMENU_H

#include "opentyr.h"

/*
 * The network game screens: Host / Join (internet with a room code, or LAN),
 * and the lobby where the host picks the episode and difficulty and starts.
 */

/** The Network Game menu.  True when a game is starting (then call net_begin_game()). */
bool netmenu_run(void);

/** Shows why a network game ended, until a key is pressed. */
void netmenu_message(const char *text);

/** The lobby of the room this computer is in.  True when the host started a game, false if the player left. */
bool netmenu_lobby(void);

#endif /* NETMENU_H */
