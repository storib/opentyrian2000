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

/*
 * tyrian-relay: the standalone relay server for internet games (src/relay.c).
 *
 *   tyrian-relay [--port=PORT] [--lan]
 */
#include "../src/netproto.h"
#include "../src/relay.h"

#include <enet/enet.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop = 0;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

int main(int argc, char *argv[])
{
	unsigned long port = NET_DEFAULT_PORT;
	bool lan = false;

	for (int i = 1; i < argc; ++i)
	{
		if (strncmp(argv[i], "--port=", 7) == 0)
		{
			char *end;
			port = strtoul(argv[i] + 7, &end, 10);
			if (*end != '\0' || port == 0 || port > 65535)
			{
				fprintf(stderr, "%s: invalid port '%s'\n", argv[0], argv[i] + 7);
				return EXIT_FAILURE;
			}
		}
		else if (strcmp(argv[i], "--lan") == 0)
		{
			lan = true;
		}
		else
		{
			printf("Usage: %s [--port=PORT] [--lan]\n\n"
			       "Relay server for OpenTyrian2000 online games (protocol version %d).\n"
			       "  --port=PORT  UDP port to listen on (default %d)\n"
			       "  --lan        also answer LAN game discovery broadcasts\n",
			       argv[0], NET_PROTOCOL_VERSION, NET_DEFAULT_PORT);
			return strcmp(argv[i], "--help") == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (enet_initialize() != 0)
	{
		fprintf(stderr, "%s: failed to initialize ENet\n", argv[0]);
		return EXIT_FAILURE;
	}

	Relay *relay = relay_create((uint16_t)port, lan);
	if (relay == NULL)
		return EXIT_FAILURE;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	while (!stop)
		relay_service(relay, 100);

	fprintf(stderr, "%sshutting down\n", relay_log_prefix);
	relay_destroy(relay);
	enet_deinitialize();
	return EXIT_SUCCESS;
}
