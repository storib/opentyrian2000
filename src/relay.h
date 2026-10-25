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
#ifndef RELAY_H
#define RELAY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The relay: rooms, input ordering and hash checking (protocol in netproto.h).
 * It builds into the game, for hosting on this computer, and into the standalone
 * tyrian-relay server.  It uses only ENet and the C library; enet_initialize()
 * must have been called first.
 */

typedef struct Relay Relay;

/**
 * Starts a relay on UDP `port`.  With `lan`, it also answers LAN discovery
 * broadcasts.  Returns NULL if the port can't be opened.
 */
Relay *relay_create(uint16_t port, bool lan);

/** Handles network traffic, waiting up to `timeout_ms` for some. */
void relay_service(Relay *relay, uint32_t timeout_ms);

void relay_destroy(Relay *relay);

/** Log lines go to stderr; this is put in front of each (e.g. "relay: "). */
extern const char *relay_log_prefix;

#endif /* RELAY_H */
