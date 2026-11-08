# Shared folders (library)

Every device has a **Shared** folder (inside the folder for received files, `…/Shared`). Its files can be browsed and
downloaded by your **contacts**: your own devices, devices you trust, and people you've exchanged messages or files
with. A device that merely appeared on the network isn't a contact and sees nothing. Turn it off in
*Library → Let my contacts download from my Shared folder*; phones don't share while on mobile data unless allowed.

## How a download works

* Files are split into 1 MB **pieces**, each with its SHA-256. A file's **id** is the SHA-256 of its piece hashes and
  size, so the same file has the same id on every device, whatever it's called there.
* Downloading asks every connected contact who has the file (`PieceHave`), gets the piece list from one of them
  (`PieceMap`, checked against the id) and then requests different pieces from each of them at the same time (up to
  8 per device in flight; at the end the last pieces are asked from several devices and the fastest wins).
* Every piece is **checked against its hash** as it arrives. A device that sends two bad pieces is dropped.
* The finished file goes into your own Shared folder, so you become a source for others. Partly downloaded files
  already offer the pieces they have. Downloads survive restarts (`swarm.json` + a `.part` file in
  `Shared/.lunashare-partial`).

## Playing without downloading

Videos and music in a contact's Shared folder have a **Play** button: the movie player opens and plays it while it
comes, without the person sharing it doing anything. It's an ordinary download with two differences: the pieces from
where the player reads come first (48 MB ahead, from every device that has the file), and nothing else is fetched.
Closing the player deletes what came. **Save a copy** (↓) on the movie screen turns it into a normal download - the
rest comes in the background and the file lands in your Shared folder.

## When it's faster

The total speed is roughly the sum of what each source can send, up to your own download speed. That helps most over
the internet, where home connections upload much slower than they download. On one Wi-Fi network the devices share
the same airtime, so several sources rarely beat one.

Everything travels over the normal end-to-end encrypted connections; nothing is stored on the server.
