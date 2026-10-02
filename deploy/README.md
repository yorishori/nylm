# Running nylm on the server

nylm runs directly on the host (Arch Linux) as a systemd service, under its
own unprivileged `nylm` user. It is reachable only from WireGuard and the
home LAN.

## Install / update

```sh
git clone <repo> ~/nylm && cd ~/nylm
sudo deploy/install.sh
```

The script installs build dependencies, builds, runs the tests, creates the
`nylm` user, installs the files, writes `/etc/nylm.conf` (first run only),
asks for the login password (first run only), and starts the service.

To update: `git pull && sudo deploy/install.sh`.

## Where things are

| Path                              | What                                       |
|-----------------------------------|--------------------------------------------|
| `/usr/local/bin/nylm`             | the binary                                 |
| `/usr/local/share/nylm/public/`   | frontend files                             |
| `/etc/nylm.conf`                  | configuration (ports, addresses, paths)    |
| `/var/lib/nylm/nylm.db`           | database (owned by `nylm`, mode 700 dir)   |
| `/usr/local/lib/nylm/actions/`    | root-owned scripts nylm may run via sudo   |
| `/etc/sudoers.d/nylm`             | the rule allowing exactly that             |
| `/etc/systemd/system/nylm.service`| the service                                |

## Who can reach it

Three independent layers:

1. **Bind:** nylm listens only on the addresses in `NYLM_LISTEN` (the
   WireGuard address and the LAN address), never on `0.0.0.0`. Do not
   port-forward nylm's port on the router.
2. **Allowlist:** connections whose source is not in `NYLM_ALLOW` (the
   WireGuard and LAN subnets) are closed before a byte is read, and logged.
3. **Login:** password + session cookie.

Traffic is plain HTTP. Over WireGuard it is encrypted by the tunnel; on the
home LAN it is not, so anyone on that network could read it. If that is a
concern, use WireGuard from the PC too and drop the LAN address from both
settings.

## What nylm can do as root

Only run programs directly inside `/usr/local/lib/nylm/actions/`, via
`sudo -n`. That folder is root-owned and not writable by `nylm`. Actions live
in `deploy/actions/` in the repo and are installed by `install.sh`. Every
action must validate its own arguments.

## Operations

| Task             | Command                                                     |
|------------------|-------------------------------------------------------------|
| Logs             | `journalctl -u nylm -f`                                     |
| Status           | `systemctl status nylm`                                     |
| Change password  | `sudo -u nylm env NYLM_DB=/var/lib/nylm/nylm.db nylm set-password` |
| Edit config      | edit `/etc/nylm.conf`, then `sudo systemctl restart nylm`   |
| Back up the DB   | `sudo sqlite3 /var/lib/nylm/nylm.db ".backup /path/nylm.db"` (needs `sqlite`) |

## Uninstall

```sh
sudo systemctl disable --now nylm
sudo rm -rf /etc/systemd/system/nylm.service /etc/sudoers.d/nylm /etc/nylm.conf \
    /usr/local/bin/nylm /usr/local/share/nylm /usr/local/lib/nylm
sudo userdel nylm            # and /var/lib/nylm if the data should go too
```
