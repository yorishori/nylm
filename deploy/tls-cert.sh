#!/bin/sh
# Makes nylm's HTTPS certificate. install.sh runs it (as root); so do the
# smoke tests.
#
#   deploy/tls-cert.sh DIR ADDRESS...
#
# DIR/ca.crt, DIR/ca.key: a certificate authority for this server alone,
# made once and then kept. A device that has ca.crt installed trusts
# nylm's certificate (see the README).
# DIR/server.crt, DIR/server.key: nylm's certificate for the IPv4
# ADDRESSes (those of NYLM_LISTEN), signed by that authority. It is made
# again when the addresses change; DIR/server.san has those it is for.
set -eu

die() { echo "${0##*/}: $*" >&2; exit 2; }

[ $# -ge 2 ] || die "usage: ${0##*/} DIR ADDRESS..."
dir=$1
shift
[ -d "$dir" ] || die "$dir is not a folder"
san=
for addr in "$@"; do
    case $addr in
        *[!0-9.]* | .* | *. | *..*) die "not an IPv4 address: '$addr'" ;;
    esac
    san="${san:+$san,}IP:$addr"
done

umask 077 # the keys: only their owner reads them
if [ ! -f "$dir/ca.crt" ]; then
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -noenc \
        -keyout "$dir/ca.key" -out "$dir/ca.crt" -days 3650 \
        -subj "/CN=nylm on $(uname -n)" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign"
    echo "made the certificate authority $dir/ca.crt"
fi

if [ -f "$dir/server.crt" ] && [ "$(cat "$dir/server.san" 2>/dev/null)" = "$san" ]; then
    exit 0
fi
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -noenc \
    -keyout "$dir/server.key" -out "$dir/server.csr" -subj "/CN=nylm" \
    -addext "subjectAltName=$san" -addext "extendedKeyUsage=serverAuth" \
    -addext "basicConstraints=critical,CA:FALSE"
openssl x509 -req -in "$dir/server.csr" -CA "$dir/ca.crt" -CAkey "$dir/ca.key" \
    -copy_extensions copyall -days 3650 -out "$dir/server.crt"
rm -f "$dir/server.csr"
printf '%s\n' "$san" > "$dir/server.san"
echo "made the certificate $dir/server.crt for $san"
