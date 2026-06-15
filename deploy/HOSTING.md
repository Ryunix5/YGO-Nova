# Hosting the EdoPro+ relay server

Online play routes both players through a **relay server** so neither needs
port-forwarding. The relay is tiny: pure-Python, no database, no accounts, and
it never touches gameplay — it just forwards bytes between the two peers in a
room. A $0–$6/month box handles many concurrent duels.

You need a public address pointing at a running relay (or host). Below: first a
**free, no-credit-card, works-anywhere** path using a tunnel (recommended if
cloud VMs aren't an option for you), then traditional always-on cloud hosting.

---

## Option 0 — Free, no card, works from anywhere: a TCP tunnel (recommended)

Cloud free tiers (Oracle/AWS/Fly) usually want a credit card and can be
region-restricted. A **TCP tunnel** avoids all of that: a small agent on any PC
connects **outbound** to a tunnel network and hands you a public address — so
CGNAT, firewalls, port-forwarding, and region blocks don't apply. No card, no
server bill.

There are two ways to use it:

### 0a. Host + tunnel — no relay server at all (simplest)

Because EdoPro+ also supports **direct host/join**, the person hosting a match
can expose their own game directly:

1. Host: main menu → **LAN MULTIPLAYER** → **Host** (it listens on a port,
   default `7878`).
2. On the same PC, run a tunnel pointed at that port. Two free options:
   - **playit.gg** (free account, persistent address, game-focused): install
     the agent, add a **TCP tunnel** to local port `7878`. It gives you a
     stable `something.playit.gg:PORT`.
   - **bore** (open-source, zero signup): `bore local 7878 --to bore.pub`
     prints a public `bore.pub:PORT` for that session.
3. Share that public `address:port`. The other player → **LAN MULTIPLAYER** →
   **Join** → paste it. Done — fully free, nothing always-on.

This needs the host's PC on **only while playing**, not 24/7.

> If direct host/join is flaky over the internet for you, use **0b** instead —
> the relay path is the one already proven to work online, just reached through
> a tunnel.

### 0b. Relay + tunnel — keeps the room browser working

If you want the **ONLINE ROOMS** lobby (strangers browsing open rooms), run the
relay behind a tunnel on whatever machine can stay on — even an **old Android
phone** with [Termux](https://termux.dev) (free, uses hardware you already own,
no cloud):

```sh
# in Termux: pkg install python, then
python relay_server.py            # listens on 0.0.0.0:7879
# in another session, run playit / bore pointed at 7879:
bore local 7879 --to bore.pub     # -> bore.pub:PORT  (or a playit.gg tunnel)
```

Players set that tunnel `address:port` as the **Server address** in
**Settings → Online**. The phone only needs to be on when people are playing.

> Tunnel trade-offs: `bore.pub` is a free shared server (port changes per run,
> occasional downtime). **playit.gg** with a free account gives a *persistent*
> address and is the better pick for a stable community.

---

## What you're running

- File: `tools/relay_server.py` (stdlib only — any Python 3.9+).
- Listens on `0.0.0.0:7879` by default.
- Config via env vars: `EDOPRO_RELAY_PORT`, `EDOPRO_RELAY_HOST`,
  `EDOPRO_RELAY_VERBOSE=1`.
- Resource use is negligible (a few MB RAM idle).

After it's up, players enter your server's **public IP + port** in
**Settings → Online** (or you can bake it in as the default — see the bottom).

---

## Option A — Free forever: Oracle Cloud "Always Free" VM

Oracle Cloud's Always Free tier includes small ARM/AMD VMs at no cost.

1. Create an **Always Free** compute instance (Ubuntu 22.04+).
2. In the instance's **VCN security list**, add an **ingress rule**: TCP, port
   `7879`, source `0.0.0.0/0`.
3. SSH in, then run the systemd steps from **Option B step 3** below.
4. Open the OS firewall too: `sudo ufw allow 7879/tcp`.

Note its **public IP** — that's your server address.

---

## Option B — Cheap VPS ($4–6/mo: Hetzner, DigitalOcean, Lightsail, Vultr)

1. Create the smallest Ubuntu VM. Note its **public IP**.
2. Open port `7879/tcp` in the provider's firewall **and** the OS firewall:
   ```sh
   sudo ufw allow 22/tcp && sudo ufw allow 7879/tcp && sudo ufw enable
   ```
3. Install the relay as a service (auto-starts on boot, restarts on crash):
   ```sh
   sudo apt update && sudo apt install -y python3 git
   sudo mkdir -p /opt/edopro-relay
   # copy relay_server.py up (scp from your machine, or clone the repo):
   sudo curl -fsSL -o /opt/edopro-relay/relay_server.py \
     https://raw.githubusercontent.com/<you>/<repo>/main/tools/relay_server.py
   sudo curl -fsSL -o /etc/systemd/system/edopro-relay.service \
     https://raw.githubusercontent.com/<you>/<repo>/main/deploy/edopro-relay.service
   sudo systemctl daemon-reload
   sudo systemctl enable --now edopro-relay
   sudo systemctl status edopro-relay     # should say "active (running)"
   journalctl -u edopro-relay -f          # live logs
   ```
   (Or just `scp tools/relay_server.py deploy/edopro-relay.service` up instead
   of the curl lines.)

---

## Option C — Docker (any container host: a VPS, Fly.io, Railway…)

From the **repo root**:

```sh
# build + run locally or on any Docker host
docker compose -f deploy/docker-compose.yml up -d
docker logs -f edopro-relay
```

Or without compose:

```sh
docker build -f deploy/Dockerfile -t edopro-relay .
docker run -d --restart unless-stopped -p 7879:7879 --name edopro-relay edopro-relay
```

**Fly.io** (free allowances, global): `fly launch` from the repo using
`deploy/Dockerfile`, expose a **TCP** service on port `7879`, deploy. Use the
app's assigned hostname as the server address.

---

## Point the game at your server

**Per player (no rebuild):** Main menu → **ONLINE ROOMS** → set **Server
address** to your public IP/hostname and **Port** to `7879`. It's remembered.

**Bake it in as the default (recommended for a public release):** rebuild with
the host baked in, so players don't have to type anything:

```sh
cmake -S . -B build/windows -DEDOPRO_DEFAULT_RELAY=relay.example.com
cmake --build build/windows --config Release
```

New installs will pre-fill that address on first run.

---

## Operating notes

- **Security:** the relay is open (anyone with the address can create/join
  rooms). That's fine for casual play. To reduce drive-by traffic, run it on a
  non-default port (`EDOPRO_RELAY_PORT=NNNNN`) and share that port.
- **Updates:** `git pull` (or re-`scp`) the new `relay_server.py`, then
  `sudo systemctl restart edopro-relay` (or `docker compose up -d --build`).
- **Health check:** `journalctl -u edopro-relay -f` should show
  `relay server started on 0.0.0.0:7879` and room create/join lines as players
  connect.
- **Protocol version:** the relay's `PROTOCOL_VERSION` must match the app's
  `kNetProtocolVersion`. Keep server and clients on the same release.
