#!/bin/sh
# Builds and installs the relay server (tyrian-relay) on an Ubuntu/Debian server
# over SSH, as a systemd service on UDP port 21900, and opens that port in ufw.
# Running it again updates the server to the relay in this checkout.
#
#   tools/deploy-relay.sh root@SERVER [SSH_KEY]
#
# SSH_KEY defaults to ~/.ssh/tyrian_relay_ed25519. Run from the repository root
# (Git Bash works on Windows).
set -eu

if [ $# -lt 1 ]; then
	echo "usage: $0 root@SERVER [SSH_KEY]" >&2
	exit 1
fi

TARGET=$1
KEY=${2:-$HOME/.ssh/tyrian_relay_ed25519}
PORT=21900
SSH="ssh -i $KEY -o StrictHostKeyChecking=accept-new -o BatchMode=yes"

cd "$(dirname "$0")/.."

echo "copying the relay sources to $TARGET"
tar czf - src/relay.c src/relay.h src/netproto.h relay/main.c |
	$SSH "$TARGET" 'rm -rf /opt/tyrian-relay/src && mkdir -p /opt/tyrian-relay/src && tar xzf - -C /opt/tyrian-relay/src'

echo "building and installing"
$SSH "$TARGET" "PORT=$PORT sh -s" <<'EOF'
set -eu

if ! dpkg -s build-essential libenet-dev pkg-config >/dev/null 2>&1; then
	# a fresh droplet is still running its own updates for a while; wait for them
	apt-get -o DPkg::Lock::Timeout=600 update -q
	DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 install -y -q build-essential libenet-dev pkg-config
fi

cd /opt/tyrian-relay/src
cc -std=c99 -O2 -Wall -Wextra -o /usr/local/bin/tyrian-relay.new relay/main.c src/relay.c $(pkg-config --cflags --libs libenet)
mv /usr/local/bin/tyrian-relay.new /usr/local/bin/tyrian-relay

cat > /etc/systemd/system/tyrian-relay.service <<UNIT
[Unit]
Description=OpenTyrian2000 relay server
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/tyrian-relay --port=$PORT
Restart=always
RestartSec=2
DynamicUser=yes
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
MemoryMax=128M

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable tyrian-relay >/dev/null 2>&1
systemctl restart tyrian-relay

# only SSH and the relay are reachable from outside
if command -v ufw >/dev/null; then
	ufw allow 22/tcp >/dev/null
	ufw allow $PORT/udp >/dev/null
	ufw --force enable >/dev/null
fi

sleep 1
systemctl --no-pager --lines=3 status tyrian-relay
EOF

echo "done: the relay is listening on $TARGET UDP port $PORT"
