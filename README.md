# nylm

A personal web app for server tools in plain C: a hand-written HTTP/1.1
server, SQLite, cJSON, OpenSSL's libcrypto for password hashing, and a static
vanilla-JS frontend. It runs on the server as a systemd service, reachable
only from WireGuard and the home LAN. See `scope.md` for the design and
`deploy/README.md` for the server side.

```sh
make run                                         # debug build on http://127.0.0.1:8080
echo 'some password' | ./nylm-debug set-password
make test                                        # unit tests + smoke test
sudo deploy/install.sh                           # on the server: install or update
```

Build needs `gcc`, `make` and OpenSSL 3.2+ headers.
