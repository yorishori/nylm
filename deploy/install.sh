#!/bin/sh
# Installs or updates nylm on this machine (Arch Linux).
#
#   sudo deploy/install.sh
#
# Run it again after `git pull` to update: it rebuilds, reinstalls the binary
# and static files, and restarts the service. Configuration (/etc/nylm.conf)
# and data (/var/lib/nylm) are kept.
#
# Addresses are detected from wg0 and the default route. Override with
# WG_CIDR=10.0.0.1/24 LAN_CIDR=192.168.1.20/24 sudo -E deploy/install.sh
set -eu

PORT=${NYLM_PORT:-8080}
WG_IF=${WG_IF:-wg0}
CONF=/etc/nylm.conf
DATA=/var/lib/nylm
SHARE=/usr/local/share/nylm
ACTIONS=/usr/local/lib/nylm/actions

step() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run with sudo"
cd "$(dirname "$0")/.."
[ -f src/main.c ] || die "run from the nylm repository"

# Build and test as the invoking user, so build/ is not owned by root.
as_user() {
    if [ -n "${SUDO_USER:-}" ]; then sudo -u "$SUDO_USER" "$@"; else "$@"; fi
}

# "a.b.c.d/len" of the first IPv4 address on an interface.
iface_cidr() {
    ip -4 -o addr show dev "$1" 2>/dev/null | awk '{ print $4; exit }'
}

step "packages"
pacman -S --needed --noconfirm gcc make openssl sqlite cjson sudo curl iproute2

step "build and test"
as_user make release
as_user make test

step "user and directories"
id nylm >/dev/null 2>&1 ||
    useradd --system --home-dir "$DATA" --shell /usr/bin/nologin nylm
install -d -m 700 -o nylm -g nylm "$DATA"
# Root-owned and not writable by nylm: this folder is nylm's whole privilege.
install -d -m 755 -o root -g root /usr/local/lib/nylm "$ACTIONS"

step "files"
install -m 755 nylm /usr/local/bin/nylm
rm -rf "$SHARE"
install -d -m 755 "$SHARE"
cp -r public "$SHARE/public"
cp README.md "$SHARE/README.md"
chmod -R a+rX,go-w "$SHARE"
if [ -d deploy/actions ]; then
    for f in deploy/actions/*; do
        [ -f "$f" ] && install -m 755 -o root -g root "$f" "$ACTIONS/"
    done
fi
install -m 644 deploy/nylm.service /etc/systemd/system/nylm.service

step "sudo rule"
tmp=$(mktemp)
cat > "$tmp" <<'EOF'
# nylm may run the programs directly inside its actions folder as root,
# and nothing else. Each action validates its own arguments.
nylm ALL=(root) NOPASSWD: /usr/local/lib/nylm/actions/
EOF
visudo -cqf "$tmp" || die "generated sudoers file is invalid"
install -m 440 -o root -g root "$tmp" /etc/sudoers.d/nylm
rm -f "$tmp"

step "configuration"
if [ -f "$CONF" ]; then
    echo "keeping existing $CONF"
else
    wg=${WG_CIDR:-$(iface_cidr "$WG_IF")}
    lan_if=$(ip -4 route show default | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
    lan=${LAN_CIDR:-$(iface_cidr "$lan_if")}
    [ -n "$wg" ] || die "no IPv4 address on $WG_IF; is WireGuard up? (or set WG_CIDR)"
    [ -n "$lan" ] || die "could not detect the LAN address (set LAN_CIDR)"
    cat > "$CONF" <<EOF
# nylm configuration, read by nylm.service. Restart after editing:
#   sudo systemctl restart nylm
NYLM_DB=$DATA/nylm.db
NYLM_PUBLIC=$SHARE/public
NYLM_PORT=$PORT
# Addresses to listen on: WireGuard and LAN only, never 0.0.0.0.
NYLM_LISTEN="${wg%/*} ${lan%/*}"
# Client subnets accepted; everything else is closed immediately.
NYLM_ALLOW="$wg $lan"
EOF
    chmod 644 "$CONF"
    echo "wrote $CONF:"
    sed 's/^/    /' "$CONF"
fi

if [ ! -f "$DATA/nylm.db" ]; then
    step "login password"
    # shellcheck disable=SC2046
    sudo -u nylm env $(grep '^NYLM_DB=' "$CONF") /usr/local/bin/nylm set-password
fi

step "service"
systemctl daemon-reload
systemctl enable nylm.service
systemctl restart nylm.service
sleep 1
systemctl --no-pager --lines=5 status nylm.service || true

step "done"
# shellcheck disable=SC1090
. "$CONF"
for addr in $NYLM_LISTEN; do
    echo "nylm: http://$addr:$NYLM_PORT"
done
