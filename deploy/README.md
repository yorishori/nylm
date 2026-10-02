# Deploying nylm

Everything runs in one container. The host provides a domain, certificates
(certbot) and a firewall.

## 1. Host prerequisites

- A domain (below: `nylm.example.com`) with an A record pointing to the server.
- Docker with the compose plugin, and `certbot` (Debian: `apt install certbot`).
- Firewall: only ports 80 and 443 (plus SSH) open.

```sh
sudo mkdir -p /srv/nylm/acme
sudo cp deploy/certbot-deploy-hook.sh /etc/letsencrypt/renewal-hooks/deploy/nylm.sh
```

## 2. First certificate

nylm needs a certificate to start, so the first one is fetched with certbot's
own temporary web server (`--standalone`) while port 80 is still free:

```sh
sudo certbot certonly --standalone -d nylm.example.com
```

The deploy hook copies the cert into `/srv/nylm/certs`. Then switch renewals
to webroot mode, so nylm can keep running while certbot renews (nylm serves
`/.well-known/acme-challenge/` from `/srv/nylm/acme` on port 80):

```sh
sudo certbot reconfigure --cert-name nylm.example.com --webroot -w /srv/nylm/acme
```

## 3. Start nylm

```sh
docker compose up -d --build
docker compose exec nylm nylm set-password     # prompts twice, no echo
```

Open `https://nylm.example.com`.

## 4. Renewals

Debian's certbot package installs a systemd timer that renews twice a day
when needed. After each renewal the deploy hook copies the new files and
runs `docker restart nylm`. Check with:

```sh
sudo certbot renew --dry-run
```

## Operations

| Task               | Command                                              |
|--------------------|------------------------------------------------------|
| Logs               | `docker compose logs -f nylm`                        |
| Change password    | `docker compose exec nylm nylm set-password`         |
| Update             | `git pull && docker compose up -d --build`           |
| Back up the DB     | see below                                            |

Backup without stopping (SQLite's online backup via a throwaway container):

```sh
docker run --rm -v nylm_nylm-data:/data -v "$PWD":/out debian:trixie-slim \
  sh -c 'apt-get update -qq && apt-get install -yqq sqlite3 >/dev/null && \
         sqlite3 /data/nylm.db ".backup /out/nylm-backup.db"'
```

OpenSSL security fixes arrive by rebuilding on a fresh base image:
`docker compose build --pull && docker compose up -d`.

## Local development

```sh
make run                  # debug build, plain http://localhost:8080
make cert && ./nylm-debug # HTTPS with a self-signed cert on https://localhost:8443
```
