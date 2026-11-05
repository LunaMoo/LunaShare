# Watch together

The ▶ button in a chat (with a person or a group) picks a movie on your device and starts watching it together.
Everyone gets an invitation in the chat (*Watch*); whoever joins sees the same moment of the movie as everyone else.
Anyone can play, pause or move to another moment, and it happens for everyone. If someone is still loading - they
just joined, someone jumped ahead, or their connection is slow - everyone waits for them ("Waiting for Kasia...")
and then all go on together. *Back* returns to the chat while the movie goes on; a group or voice call works at
the same time (mute and hang up are on the movie screen).

## How it works

- Nothing is sent before watching. The movie stays on the device that shared it; everyone else's player reads it
  through the connection: the parts it needs, in 256 KB blocks, up to 32 MB ahead of where it plays. What arrived is
  kept in a temporary file, deleted when you leave. It's the original file - no re-encoding, the same quality, and
  no work for the phone that shares it beyond reading the file.
- The device that shared the movie keeps the clock (playing / paused, and the position). Viewers send it their
  play / pause / seek; it tells everyone. Every second it sends the clock again; players more than a second off jump
  to it. Viewers tell it whether they have enough loaded (4 MB ahead after waiting); while someone doesn't, the clock
  stands still.
- It needs a connection to the device with the movie (on the same network, or over the internet - directly or relayed
  by your server). A movie the player can play from a file can be watched together (on phones: what Android plays).

## Messages

`WatchInvite {sid, name, size[, gid]}`, `WatchJoin`, `WatchLeave {end}`, `WatchCtl {playing, pos}`,
`WatchState {playing, pos, hold, wait, people}`, `WatchReport {buffering}`, `WatchRead {off, len}` and the binary
`WatchData [sid u64][offset u64][data]` (sent before other bulk data on the connection). The player module reads
through `open_stream()` (player API 2): FFmpeg with a custom AVIOContext on computers, a `MediaDataSource` on Android.
