#!/bin/sh
# Installs or updates nylm on this machine (Arch Linux).
#
#   sudo deploy/install.sh
#
# Run it again after `git pull` to update: it rebuilds, reinstalls the binary
# and static files, and restarts the service. Configuration (/etc/nylm.conf)
# and data (the NYLM_DATA folder) are kept.
#
# All data lives in NYLM_DATA, a folder you create on the data drive and make
# writable by user nylm. Use a folder INSIDE the drive (e.g. /mnt/data/nylm),
# not the mount point: if the drive is not mounted the folder is missing and
# nylm refuses to start, instead of starting empty on the system disk.
# The first install asks for it, or takes NYLM_DATA from the environment.
#
# Addresses are detected from wg0 and the default route. Override with
# WG_CIDR=10.0.0.1/24 LAN_CIDR=192.168.1.20/24 sudo -E deploy/install.sh
set -eu

PORT=${NYLM_PORT:-8080}
WG_IF=${WG_IF:-wg0}
CONF=/etc/nylm.conf
HOME_DIR=/var/lib/nylm # nylm's home and working directory; holds no data
SHARE=/usr/local/share/nylm
ACTIONS=/usr/local/lib/nylm/actions
JOBS=/usr/local/lib/nylm/jobs

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
pacman -S --needed --noconfirm gcc make openssl sqlite cjson taglib libjpeg-turbo libpng \
    smartmontools wireguard-tools pacman-contrib fakeroot zstd rsync \
    sudo curl iproute2 util-linux

step "build and test"
as_user make release
as_user make test

step "user and directories"
id nylm >/dev/null 2>&1 ||
    useradd --system --home-dir "$HOME_DIR" --shell /usr/bin/nologin nylm
install -d -m 700 -o nylm -g nylm "$HOME_DIR"
# Root-owned and not writable by nylm: this folder is nylm's whole privilege.
# The jobs (root scripts run by systemd units) and lib.sh sit next to it,
# where sudo does not reach.
install -d -m 755 -o root -g root /usr/local/lib/nylm "$ACTIONS" "$JOBS"

step "files"
install -m 755 nylm /usr/local/bin/nylm
install -m 755 nylm-qobuz /usr/local/bin/nylm-qobuz
rm -rf "$SHARE"
install -d -m 755 "$SHARE"
cp -r public "$SHARE/public"
cp README.md "$SHARE/README.md"
chmod -R a+rX,go-w "$SHARE"
for f in deploy/actions/*; do
    install -m 755 -o root -g root "$f" "$ACTIONS/"
done
for f in deploy/jobs/*; do
    install -m 755 -o root -g root "$f" "$JOBS/"
done
install -m 644 -o root -g root deploy/lib.sh /usr/local/lib/nylm/lib.sh
for f in deploy/*.service; do
    install -m 644 "$f" /etc/systemd/system/
done

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

step "data folder"
if [ -f "$CONF" ]; then
    # shellcheck disable=SC1090
    NYLM_DATA=$(. "$CONF" && printf '%s' "${NYLM_DATA:-}")
    [ -n "$NYLM_DATA" ] || die "$CONF has no NYLM_DATA; add NYLM_DATA=/path/on/drive"
elif [ -z "${NYLM_DATA:-}" ]; then
    [ -t 0 ] || die "set NYLM_DATA to the data folder on the drive"
    printf 'data folder on the drive (e.g. /mnt/data/nylm): '
    read -r NYLM_DATA
fi
# Only plain absolute paths: the value goes into a file that systemd parses
# and this script sources.
case "$NYLM_DATA" in
    /*) ;;
    *) die "NYLM_DATA must be an absolute path: '$NYLM_DATA'" ;;
esac
case "$NYLM_DATA" in
    *[!A-Za-z0-9/._-]*) die "NYLM_DATA may only contain A-Z a-z 0-9 / . _ -" ;;
esac
[ -d "$NYLM_DATA" ] || die "$NYLM_DATA does not exist (create it on the drive; is it mounted?)"
sudo -u nylm test -w "$NYLM_DATA" -a -x "$NYLM_DATA" ||
    die "user nylm cannot write to $NYLM_DATA (chown it, or mount with uid=nylm)"
echo "data: $NYLM_DATA"

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
# Data folder on the drive; each app has a subfolder and database in it.
NYLM_DATA=$NYLM_DATA
NYLM_PUBLIC=$SHARE/public
NYLM_PORT=$PORT
# Music library folder for the music app; user nylm must be able to read
# and write it. Leave it commented out to not use the music app.
#NYLM_MUSIC=/mnt/data/music
# Addresses to listen on: WireGuard and LAN only, never 0.0.0.0.
NYLM_LISTEN="${wg%/*} ${lan%/*}"
# Client subnets accepted; everything else is closed immediately.
NYLM_ALLOW="$wg $lan"
# Server app (see the README): more systemd units to show; the backups:
# entries name=/path[,/path...], and the folder they are written to.
#NYLM_UNITS="wg-quick@wg0 docker sshd"
#NYLM_BACKUP="davis=/var/lib/docker/volumes/davis_data/_data"
#NYLM_BACKUP_DIR=/mnt/data/backups
#NYLM_BACKUP_KEEP=2
#NYLM_BACKUP_GROUP=you
EOF
    chmod 644 "$CONF"
    echo "wrote $CONF:"
    sed 's/^/    /' "$CONF"
fi

step "music folder"
# shellcheck disable=SC1090
NYLM_MUSIC=$(. "$CONF" && printf '%s' "${NYLM_MUSIC:-}")
if [ -z "$NYLM_MUSIC" ]; then
    echo "NYLM_MUSIC is not set in $CONF: the music app is off"
elif sudo -u nylm test -d "$NYLM_MUSIC" -a -r "$NYLM_MUSIC" -a -w "$NYLM_MUSIC" -a -x "$NYLM_MUSIC"; then
    echo "music: $NYLM_MUSIC"
else
    # Not fatal: the folder may be on a drive that is not mounted right now.
    echo "warning: user nylm can not read and write $NYLM_MUSIC; the music app" \
         "will show it as not available"
fi

if [ ! -f "$NYLM_DATA/core/core.db" ]; then
    step "login password"
    sudo -u nylm env NYLM_DATA="$NYLM_DATA" /usr/local/bin/nylm set-password
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
