# Using LunaShare over the internet

LunaShare works on the local network with no setup. To reach your devices from anywhere (phone on mobile data, a
laptop at a friend's place, ...) you can run the small, optional **rendezvous server**, for example on your
OPNsense router.

## What the server does (and doesn't)

- Devices that use the same **room name + password** register with the server and learn each other's public
  addresses. The server only sees a hash of the room name and password, never the password itself.
- When two devices want to talk, the server tells both to send packets to each other at the same moment
  ("UDP hole punching"). From then on they talk **directly, peer-to-peer**, with the same end-to-end encryption as on
  the LAN (X25519 + XChaCha20-Poly1305, pinned device keys). KCP gives reliable delivery over UDP.
- The server **never relays chat or files** and can't read them. If it goes down, existing connections keep working.
- Stored per device, in memory only: ID, display name, platform, public key, public and LAN addresses. Entries
  expire 60 s after the device goes away.

## Server on OPNsense

1. Over SSH (as root, option 8 "Shell" in the console menu), download and install:
   ```sh
   cd /tmp
   fetch https://github.com/LunaMoo/LunaShare/releases/download/latest/lunashare-opnsense.tar.gz
   tar xzf lunashare-opnsense.tar.gz && cd lunashare-opnsense && sh install.sh
   ```
   To update later, run the same commands again.
2. In the web UI: **Services → LunaShare rendezvous**, tick *Enable*, keep port **47830** (UDP), leave
   *Allow on WAN* ticked (adds the firewall pass rule for you), press **Apply**. The service also appears in the
   Services widget / dashboard with start/stop buttons. Logs: *System → Log Files → General* (tag `lunashare`).
3. Optional: paste your room hash(es) into *Allowed rooms* so strangers can't use your server.

Uninstall: `sh install.sh uninstall`.

## Server anywhere else

The same program runs on Linux/macOS/Windows: `lunashare-rendezvous -p 47830` (see `--help`). For Linux there's a
systemd unit in `server/lunashare-rendezvous.service`. Open UDP 47830 in front of it.

## In LunaShare (each device)

*Settings → Internet (optional)*:
- **Server:** your public address or DynDNS name plus port, e.g. `home.example.com:47830`
- **Room name** and **room password:** the same on all your devices
- Optional: *Copy room hash* to put it on the server's allow-list.

Devices in the room show up in the device list as *online (internet)* and connect automatically when needed (if
*Connect to found devices automatically* is on), or when you open the chat. The chat header shows *via Internet*.
If a device is also on the same LAN, the LAN connection is preferred.

## When the direct connection fails

Hole punching works with most home routers and many mobile networks. It fails when **both** sides are behind NATs that
pick a new random port for every destination: carrier-grade NAT on some mobile networks, and pf-based firewalls
like OPNsense/pfSense with default outbound NAT. Fixes, any one is enough:

- **NAT-PMP** (on by default in LunaShare): the device asks its router for a port mapping. On OPNsense install
  **os-upnp**, enable it with *Allow NAT-PMP port mapping*, and restrict it to your LAN. Many home routers support it too.
- **Forward a port:** in *Settings → Internet → Connection help* set a fixed local UDP port (e.g. 47831) and
  forward that UDP port on your router to the device. Enter the external port as *Port forwarded on the router*.
- On OPNsense you can also make outbound NAT keep source ports for the device (Firewall → NAT → Outbound,
  hybrid mode, a rule with *Static port* for that device's address).

Devices on the same LAN as the server: register using the router's public name like everyone else; when the other
side is outside, at least one of the fixes above is usually needed on the OPNsense side, because pf NAT is
port-randomising.

Not supported yet: IPv6 candidates, and relaying through the server when no direct path exists.
