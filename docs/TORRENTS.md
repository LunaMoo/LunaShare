# Torrents

LunaShare downloads torrents on **computers** (Windows and Linux) with its optional torrent module
(`modules/lunashare_torrent.dll` / `liblunashare_torrent.so`: [libtorrent](https://libtorrent.org) with OpenSSL
inside). Delete that file and LunaShare has no torrents; nothing else changes. Phones don't download torrents
themselves - they use a computer of yours (below).

## On the computer

The **Torrents** tab: *Add a torrent...* takes a **magnet link** (typed or pasted) or a **.torrent file**; both can also
be opened with LunaShare from the browser or the file manager (*Settings → Opening files* registers `.torrent` and
`magnet:`), or given on the command line. Pick the folder (by default `Torrents` in the folder for received files)
and it starts.

Each torrent shows how far it is, the speed, the time left and the people connected, with:

* **Pause / Resume**; once done: *Stop sharing* / *Share again*.
* **Watch** - the biggest movie inside plays in the movie player **while it still downloads**: the pieces under the
  place being watched are fetched first (the beginning and the end right away), the picture waits when they aren't
  there yet, and seeking jumps the download to the new place. A finished file plays like any other.
* **Files...** - which files of the torrent to download.
* **Open folder**, **Send...** (to a chat, when done), **Remove...** (optionally with its files).

*Settings...*: the folder, download / upload limits (KiB/s), how long a finished torrent is shared with others
(stop when downloaded - as much uploaded as downloaded (the default) - twice as much - until stopped by hand), the
port (0 = any; it's opened in the router by UPnP / NAT-PMP when the router allows), and whether your other devices may
add torrents here.

Finished files land in the folder for received files, so the Gallery, Movies, Music and Archives find them.
Torrents go on after a restart (`torrents/` in the settings folder keeps what's downloaded so far). DHT, magnet
links, UDP / HTTP / HTTPS trackers, peer exchange, local peer discovery and encrypted connections are on.

## From a phone: "download on my computer"

A phone (or any device without the module) linked to the **same account** as the computer works as its remote:

* A **magnet link** tapped in the browser, or a **.torrent** file opened, offers LunaShare. It asks which
  computer (when there are several) and what to do when it's done: **send it to this device**, or **keep it on the
  computer**. When the computer is offline the link is kept and handed over when it comes online.
* The phone's **Torrents** screen (home screen, Library) shows that computer's torrents: progress, pause, remove,
  **Send here** for a finished one - and **Watch**, which plays the movie from the computer, also while it still
  downloads there (read in 256 KB blocks over the encrypted connection between your devices).
* Files sent to the phone arrive like a folder sync from your own device: without asking, into the folder for
  received files.

This goes over LunaShare's own-device channel (`OwnCmd`, only between devices of one account - a contact can't ask
your computer for anything). The computer can refuse it: *Torrents → Settings... → My other devices can add torrents
here*.

## Notes

* Downloading and sharing what you have no right to is your responsibility; a torrent shares the pieces you have
  with other people while it runs.
* On Linux the module links OpenSSL statically and looks for the system's certificates in the usual places.
* Building: `-DLUNASHARE_BUILD_TORRENT=OFF` leaves the module out. It fetches Boost's headers (about 47 MB) and
  libtorrent; OpenSSL comes from the system (`libssl-dev`, or `OPENSSL_ROOT_DIR` on Windows) - without it the module
  is built without HTTPS trackers.
