# Music player

*Music* (a tab on computers; *Music player* on a phone's main screen) plays the music on this device.

## Library

- **Folders:** *Folders... → Add a folder...* adds a folder with everything under it. The folder for received files
  is always included, so music people send you - or that you download from their Shared folders - shows up by itself.
  The folders are looked through again when something new arrives, and with *Look again*.
- **Tags:** title, artist, album, album artist, genre, year, track and disc number, length - read from the files
  (MP3 ID3, MP4/M4A, FLAC and Ogg/Opus comments...) by the player module. Without tags the file and folder names are
  used.
- **Covers:** the picture inside the file (kept once per picture in `music_covers/` in the settings folder), or
  `cover.jpg` / `folder.jpg` / `front.jpg` lying next to the music.
- **Views:** Songs (sorted by title, artist, album or recently added), Artists, Albums, Genres, Folders and Playlists,
  with a search over title, artist, album and genre.
- The library is kept in `music.json`; what was playing (the queue, the position, repeat and shuffle) in
  `music_state.json`, so it can go on after a restart.

## Playing

- Tapping a song plays the list it's in from there (an album, an artist, a playlist, the search result...).
- A song's menu (right-click, or long-press on a phone): play next, add to the queue, add to a playlist, send,
  select (then send or add all selected at once).
- **Repeat:** off → the list → this song. **Shuffle:** a random order where every song plays once before any comes
  again.
- The **mini player** at the bottom (above the phone's navigation, in the desktop sidebar) shows the song with play /
  pause and next; tapping it opens the player.
- Music pauses on its own for a movie, a chat video or voice message, a call or a recording, and goes on afterwards.

## In the background

- **Android:** the music plays on with the app in the background or the screen off. The notification (and the lock
  screen) shows the cover and title with previous / play-pause / next; headset and Bluetooth buttons work; a phone
  call or another app playing pauses it (it comes back after the call), navigation prompts make it quieter for a
  moment, and pulling out headphones pauses it. Swiping the notification away (when paused) stops the player.
- **Windows:** it plays on when the window is minimized or closed to the tray; the keyboard's media keys (play /
  pause, next, previous, stop) work from anywhere while music is loaded.
- **Linux:** it plays on when minimized.

## Sending

*Send...* on a song, a selection, an album, an artist or a playlist sends it to a person or a group like any files
(resumable, checked with SHA-256). An album, an artist or a playlist goes as one folder named after it, with the
album's cover picture when there is one next to the music; single songs and selections go as files.
