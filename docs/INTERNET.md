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
4. Optional, **links for anyone** ([LINKS.md](LINKS.md): people without LunaShare download a file in a browser):
   tick *Links for anyone*, pick a certificate for your public name under *Links: certificate* (get one with the
   ACME client plugin, Let's Encrypt), keep port **47831** (TCP; not the port of the web interface), press
   **Apply**. *Allow on WAN* then also opens TCP 47830 and 47831. After the certificate is renewed, restart the
   service (an ACME automation can do it).

Uninstall: `sh install.sh uninstall`.

## Server anywhere else

The same program runs on Linux/macOS/Windows: `lunashare-rendezvous -p 47830 -d /var/lib/lunashare` (see `--help`). For Linux there's a
systemd unit in `server/lunashare-rendezvous.service`. Open UDP 47830 in front of it. For links for anyone add
`--link-port 47831 --cert ... --key ...` and open TCP 47830 and 47831 too (see [LINKS.md](LINKS.md)).

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

## Private contacts (people who mustn't find each other)

Everyone in a room sees everyone else in it. To stay in touch with people who must never learn about each other,
invite them with a code instead of giving them the room:

1. *Settings → Invites* (also: click your own name at the top of the device list; on phones *Invite someone / use an invite code* on the main screen): type who it is for (only you see that) and press **Create invite code**.
   The code is copied; send it to that one person. Make a new code for each person.
2. They paste it under *invite code you got* and press **Use code**. If they hadn't set up the internet yet, this
   also sets the server and turns on **private mode** for them.
3. When both of you are online you see each other in the device list and connect as usual (the safety number still
   tells whether it is really them).

**Invite rooms:** when creating a code, pick a room (or *New room...* and give it a name - the people you invite see
it). Everyone invited into the same room sees the others in their device list, and you; people without a room or in
a different room still see only you. *Delete room* keeps everyone as your private contact, they just stop seeing each
other. Removing one person's code takes them out of the room too.

How it works: both devices send the server a hash of the code's secret. Your device also tells the server which of
its codes belong to which room (as hashes, with the room's name so the server's admin can see it). The server introduces devices that share one,
whatever room they are in, and never lists them to anyone else. Two people with different codes from you share
nothing, so the server never puts them in touch. **Private mode** keeps a device out of its room's list (except
for the same account's devices); it still sees the room and connects automatically only to devices it already knows.
Offline messages to a private contact go to a mailbox only that device can open. *Remove* deletes a code: that person
can no longer find you through the server.

On a device that joined with an invite, the app doesn't show the server's address, rooms or usernames and can't make
invite codes to it; that device gets a room of its own, so it sees no one on the server but you. (This is hiding, not
secrecy: the device has to know the address to connect, so a determined person can still find it, e.g. by
decoding the invite code or watching their network traffic.)

**The server remembers who joined with an invite** (by device key, and by account so their other devices count
too), in `invited.json` in its data directory. Such a device stays limited even if the person later removes the
invite and enters the server's address by hand: it sees and reaches only the people whose invites it used and its own
account's devices, is never listed in any room, can't use usernames, and codes it makes for this server don't work.
The list shows each device's name, who invited it (their name and device key) and the invite room, if any.
To treat someone like everyone else, release them: on OPNsense under *Services → LunaShare rendezvous → People who
joined with an invite code*; elsewhere with `lunashare-rendezvous -d DIR --list-invited` and
`lunashare-rendezvous -d DIR --release KEY` (`--restrict KEY` limits them again). A running server applies the change
within a few seconds; releasing one releases the whole account and every install on that computer or phone.

Devices also send the server a fingerprint of the computer or phone itself: a hash of the system's own id (Windows
MachineGuid, Linux machine-id, macOS hardware UUID, Android ANDROID_ID) together with the server's address, so
different servers can't match devices with each other. A new install, a new account or a new device key on a
remembered computer or phone is therefore limited too (and listed, so the admin sees it). What resets it: reinstalling
the operating system or a phone's factory reset. A modified copy of the app could also send a different fingerprint:
it keeps honest installs in line, it can't stop someone determined.

Things that still connect people: putting them in the same group, and being on the same local network (they then find
each other there like any devices). Private contacts need an up-to-date server (1.2.5 or later); the settings show a
warning when it is older.

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
