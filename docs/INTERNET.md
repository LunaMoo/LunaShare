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
- Only if no direct path works (see below) does the server **relay** the packets between the two devices. They
  are still end-to-end encrypted, so the server can't read chat or files. Relaying can be turned off on the server
  (`--no-relay`, or the OPNsense checkbox) and on each device. If the server goes down, direct connections keep working.
- Stored per device, in memory only: ID, display name, platform, public key, public and LAN addresses. Entries
  expire 60 s after the device goes away.
- **Offline messages:** a text message for a device that isn't reachable waits in the server's mailbox until that
  device comes online (also delivered/read receipts). Each one is sealed for the receiving device's key: the server
  sees neither the text nor who sent it, only which device key it is for, when, and its size. Only members of the room
  can leave messages, only the owner of the key can take them out (it signs a challenge), and they are deleted once
  delivered, or after 10 days. Limits: 10 MB per receiving device, 1 GB in total (`--mailbox-days`, `--mailbox-mb`,
  `--mailbox-total-mb`, `--no-mailbox`). With `-d DIR` they survive server restarts (the OPNsense plugin uses
  `/var/db/lunashare`). Files are never stored; they wait until both devices are online.
- **Usernames:** a person can claim a username in the room (signed with their account key; first come, only that
  account can change or release it). Others in the room can look it up and get the account key and its online
  devices, then connect as usual - the safety number still tells whether it is really them. For this, each device
  tells the server which account it belongs to (with the account's signature).

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

The same program runs on Linux/macOS/Windows: `lunashare-rendezvous -p 47830 -d /var/lib/lunashare` (see `--help`). For Linux there's a
systemd unit in `server/lunashare-rendezvous.service`. Open UDP 47830 in front of it.

## In LunaShare (each device)

*Settings → Internet*: enter the **server** (your public address or DynDNS name, e.g. `home.example.com`; add
`:port` only if it isn't 47830). That's all - everything else works automatically.

Under *Settings → Advanced → Internet*:
- **Room name** and **room password:** the same on all your devices. Without them, every device using that server is
  in one shared room; set them on a server other people use.
- *Copy room hash* to put it on the server's allow-list.

Devices in the room show up in the device list as *online (internet)* and connect automatically when needed (if
*Connect to found devices automatically* is on), or when you open the chat. The chat header shows *via Internet*.
If a device is also on the same LAN, the LAN connection is preferred.

## When the direct connection fails

Hole punching works with most home routers and many mobile networks. It fails when **both** sides are behind NATs that
pick a new random port for every destination: carrier-grade NAT on some mobile networks, and pf-based firewalls
like OPNsense/pfSense with default outbound NAT. Fixes, any one is enough:

- **UPnP / NAT-PMP** (on by default in LunaShare): the device asks its router for a port mapping. Most home routers
  allow this out of the box (UPnP). On OPNsense install **os-upnp**, enable it (UPnP and/or *Allow NAT-PMP port
  mapping*), and restrict it to your LAN.
- **Relay** (on by default, nothing to set up): when no direct path is found within about five seconds, the server
  carries the packets. This is what makes it work on networks you can't configure at all (a router you have no
  access to, carrier-grade NAT, a hotel or office firewall). It is slower than a direct connection and uses the
  server's bandwidth. The chat header shows *via Internet (relay through the server)*.
- **Forward a port:** in *Settings → Advanced → Internet* set a fixed local UDP port (e.g. 47831) and
  forward that UDP port on your router to the device. Enter the external port as *Port forwarded on the router*.
- On OPNsense you can also make outbound NAT keep source ports for the device (Firewall → NAT → Outbound,
  hybrid mode, a rule with *Static port* for that device's address).

Devices on the same LAN as the server: register using the router's public name like everyone else; when the other
side is outside, at least one of the fixes above is usually needed on the OPNsense side, because pf NAT is
port-randomising.

Not supported yet: IPv6 candidates.
