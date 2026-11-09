# Accounts, profiles and linked devices

LunaShare has no sign-up and no server-side accounts. An **account is a key that belongs to you**. Every device creates
one the first time it starts (older installs get one on update), and more of your devices can be **linked** to it.

## What is what

| Thing | Key | Where it lives |
|---|---|---|
| Device identity | EdDSA key of one device (unchanged since 1.0) | that device only |
| Account | EdDSA key of the person | every linked device; the recovery phrase / backup file |
| Device certificate | the account key's signature over `device id + device key` | sent in every Hello |
| Device list | the account's signed list of its devices (+ removed ones), with a version | every linked device; contacts cache it |
| Profile | name + picture (SHA-256), signed by the device key | sent on connect, cached by contacts |

Contacts trust the **account**: a new device of someone you know (valid certificate) joins that person's conversation
without a new "safety number" warning, and verifying one of their devices verifies the person. The safety number shown
for a person is computed from the two **account** keys.

## Conversations

A conversation's key is `@` + the person's account key. Messages to a person go to **each** of their devices
(`pendingTo` lists who still has to get a message; it is resent when that device connects). A copy goes to **my own**
other devices (`ChatSync`), so what I write on the phone also appears on the computer, and messages I receive are
forwarded the same way in case the sender did not know all my devices yet. Message ids keep everything free of
duplicates. Files and calls go to one device of the person: the connected one with the fastest link.

Devices of older versions (no account) keep a conversation of their own, keyed by device id, as before.

## Linking a device

1. Both devices are connected (LAN, hotspot, Bluetooth or the internet server).
2. On the device that has the account: *Settings → Your account → Link another device…*, pick the other device.
3. The other device asks *"Link this device?"*. Both show the same 6-digit code, derived from this connection's
   key exchange; a man in the middle would produce different codes.
4. After *Link*, the account device adds the new device to its signed device list and sends it the account key, the
   person's name and picture, contacts, other people's device lists and the conversations (newest 2000 messages each).
5. The new device reconnects everywhere; contacts learn about it from its certificate and the pushed device list.

## The same playlists and settings everywhere

*Settings > Your account > The same playlists and settings on my devices* (on by default). Linked devices keep
these the same whenever they are connected to each other:

- music playlists - each one separately: the later change wins, a deleted one stays deleted. Songs are matched by
  file name and size, then by artist and title, so they're found wherever they are on each device; a song that isn't
  on a device stays in the playlist (hidden) and shows up once it's there.
- the sound settings (equalizer, transitions, remix, volume levelling), the movie player's options (autoplay, next,
  hiding the controls, rotation, small window, subtitle size), silent chats and the language - as one piece, the
  later change wins.

*Share the clipboard with my devices* (off by default - a clipboard often holds passwords): text copied on one
linked device can be pasted on the others while LunaShare is open (on a phone: in front - Android doesn't let apps
read the clipboard from the background). A device that comes online gets a clipboard copied in the last 10 minutes.

*Folders sent to my devices by themselves*: pick a folder and one of your devices. New and changed files go there
by themselves (looked at every minute and when that device comes online; a file still being written waits) - into
its download folder, in a folder of the same name, accepted without asking because they come from your own device.
Nothing is deleted on the other side; a file changed later arrives again as a new copy. Hidden files (starting with
`.`) and unfinished downloads are skipped. What was sent is kept in `folder_sync.json`.

Not synced: how a screen looks (theme, size), folders and ports - those belong to each device. (Message type 104
`SyncData` `{kind, data}`, only between devices of the same account.)

## App lock

*Settings > App lock > Set a PIN...* (4 - 12 digits): LunaShare asks for it when it starts and when you come back
to it - right away, or after 1 / 5 / 15 / 60 minutes away. On Android 10+ the fingerprint / face can unlock it too.
While it's locked, messages and files keep arriving, notifications say only *New message*, an incoming call can
still be answered, and on Android the app switcher shows no preview (and screenshots are blocked). Five wrong PINs
make it wait 30 s (then longer). *Forgot the PIN?* - the account's recovery phrase opens it and turns the lock off.

It keeps out someone holding your unlocked device; it doesn't encrypt the files on it. The PIN is stored salted
and hashed (20 000 rounds of SHA-256).

## Recovery

* **Recovery phrase**: the 32-byte account key as 24 words (BIP-39 English word list, 8-bit SHA-256 checksum). Restoring
  from it brings back the account; contacts and conversations come back from the account's other devices.
* **Backup file** (`.lunabackup`): the same data a linked device receives, encrypted with a password
  (Argon2id, 32 MiB, 3 passes → XChaCha20-Poly1305).

A restored device adds itself to the device list. Devices that hold the account key and are not on the list's
`removed` part add themselves back as well, so restoring on a new phone never pushes the computer out.

**Removing a device** puts it on the `removed` list; when it hears the new list it starts an account of its own.
This relies on the removed device's honesty: it still knows the account key. Taking the key away from a stolen
device needs a new account key ("reset account"), which is not implemented yet.

## Offline messages and prekeys (planned)

The server mailbox (next step) stores messages for devices that are not reachable, for up to 10 days and 10 MB per
person, text only. Each message is sealed for one recipient device:

* **Version 1 – sealed to the device key.** `X25519(ephemeral, recipient device key)` → XChaCha20-Poly1305, plus the
  sender's signature inside the ciphertext (so the server can't see or forge the sender). Simple, works with any
  device that was ever seen. No forward secrecy: if a device key is stolen later, stored messages addressed to it
  can be read.
* **Version 2 – prekeys** (Signal's X3DH idea). Each device uploads to the server a signed medium-term prekey and a
  batch of one-time prekeys (X25519), signed by its device key. A sender takes one one-time prekey (the server hands
  each out once) and combines `DH(sender identity, signed prekey)`, `DH(ephemeral, recipient identity)`,
  `DH(ephemeral, signed prekey)` and `DH(ephemeral, one-time prekey)`. The recipient deletes the one-time private key
  after use, so a later key theft can't decrypt old messages. When the server runs out of one-time prekeys, the
  signed prekey alone is used (still rotated weekly).

The message envelope carries a version byte and the ids of the keys used, so v2 can be added without changing the
mailbox or v1 messages. A full double ratchet is not needed: live chats already use fresh keys per connection; only
offline messages need the prekey protection.
