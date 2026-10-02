#!/bin/sh
# certbot --deploy-hook: runs after every successful issue/renewal.
#
# certbot keeps private keys readable by root only, but the container runs
# as uid 10001. Copy the cert and key to /srv/nylm/certs, readable by that
# group, then restart the container so it loads them.
set -eu

dest=/srv/nylm/certs
install -d -m 750 -o root -g 10001 "$dest"
install -m 644 -o root -g 10001 "$RENEWED_LINEAGE/fullchain.pem" "$dest/fullchain.pem"
install -m 640 -o root -g 10001 "$RENEWED_LINEAGE/privkey.pem" "$dest/privkey.pem"

if docker inspect nylm >/dev/null 2>&1; then
    docker restart nylm
fi
