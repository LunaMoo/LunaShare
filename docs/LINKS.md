# Links for anyone

Send a file to someone who doesn't have LunaShare: make a link, send it by SMS, e-mail or any messenger, and they
download the file in their web browser. Nothing is uploaded anywhere first - the file comes **from your device**,
so it works while LunaShare runs there.

## Making a link

Wherever you can send something (a file in a chat, the file picker, the gallery, an archive, a torrent...), choose
*Send to...* and then **Link for anyone...** at the bottom. Several files or a folder go as one `.zip` (packed
first). The window shows the link, **Copy**, **Share...** (phones) and a **QR code**. Under *Options*:

- **Valid for** 1 hour, 1 day (default) or 1 week.
- **For** one person (default), 5 people, or anyone with the link. A "person" is one browser that started the
  download: it can resume or download again; someone else opening the link afterwards is told it was used.
- **PIN** (optional): asked on the page; tell it separately, not in the same message. 5 wrong tries end the link.
- **Ask me before each download:** whoever opens the link waits until you allow it (a window pops up, or a
  notification on a phone). Unanswered for 10 minutes = no.

Your links are listed under *Files → Transfers* while there are any: how long each still works, who downloaded it
(hover or tap the name), **copy** the link again or **turn it off**. Ended links disappear after a day.

## Two kinds of link

**On the same network** (`http://192.168.1.20:47823/f/...`): the page and the file come straight from your device.
Only people on the same Wi-Fi / LAN can open it. That's the link you get when your server doesn't offer links.

**Over the Internet** (`https://your.server:47831/s/AbCdEfGhIjKl#...`): when your [rendezvous
server](INTERNET.md) offers links for anyone. The page comes from the server; the file comes from your device
through a connection it keeps to the server while you have links. The link is short (about 70 characters).

## Is it safe?

- The link can't be guessed (72 random bits for its name, 128 for its key) and ends when you say.
- **End-to-end encrypted.** Everything after `#` in an Internet link is the key; browsers never send that part
  anywhere. Your device encrypts the file (and its name) with it in 1 MB parts (AES-256-CTR); the server passes on
  what it can't read and never learns the file's name or contents.
- **Every part is checked in the browser** (HMAC-SHA-512 with its position, and which part is the last): a part
  changed on the way, out of order, or a file cut short is refused and fetched again. The page then says
  *"Downloaded and checked"* and shows the file's SHA-256 (for files up to 128 MB it also checks that). The person
  on the other side doesn't have to compare anything.
- The PIN is checked by your device (the page sends a proof made with the key, not the PIN in the clear).
- The page doesn't load anything from anywhere else, is never indexed, and passes no referrer.
- If the file changes on your device after you made the link, the link stops working (make a new one).
- Same-network links are plain HTTP on your own network - fine at home; on public Wi-Fi prefer the PIN or "ask me".

Big files: in browsers that can (Chrome, Edge, Firefox; not private windows), files over 256 MB are written straight
to disk while they come; smaller ones (and in other browsers, everything) are put together in memory first.

## The server's side

Enable it on the server (OPNsense: *Services → LunaShare rendezvous → Links for anyone*, see
[INTERNET.md](INTERNET.md)):

```sh
lunashare-rendezvous -p 47830 -d /var/lib/lunashare --link-port 47831 --cert fullchain.pem --key privkey.pem
```

- `--link-port`: the HTTPS port of the pages. Devices connect to **TCP** on the rendezvous port (`-p`), so open
  TCP 47830 and TCP 47831 as well as UDP 47830.
- `--cert` / `--key`: a certificate for the server's public name (e.g. Let's Encrypt). It's read again when the
  files change.
- Behind a reverse proxy instead: `--link-port 8080 --link-url https://share.example.com` (plain HTTP on that port,
  keep it closed to the outside; the visitor's address comes from `X-Forwarded-For`).
- Only devices registered in a room may offer links - not devices limited by an invite. Nothing is stored; each
  browser is limited to a modest number of requests per second.
