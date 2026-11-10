# Archives

The **Archives** page (*Tools → Archives*; on phones a tile in *Library*) lists the archives in *Downloads*,
*Documents*, the received files folder and the folders you add (*Folders...*), newest first. *Open...* opens any
other one; opening an archive from the file manager (*Open with*) or from a chat lands here too.

Inside an archive: its folders (one folder at the top is opened for you), sizes, and the format.

* Tap a folder to go in (the path above goes back); tap a file to **open** it - it's unpacked to a cache folder and
  shown where it belongs (pictures in the viewer, movies and songs in their players, archives here again).
* Tick files and folders, then **Unpack picked...** or **Send...** (unpacked to the cache, then sent to a chat).
* **Unpack all...** - next to the archive, into a folder named after it (or anywhere else).
* Archives with a **password** (zip, 7z, rar) ask for it.

**Pack...** puts files and folders into a new archive: **zip** (opens everywhere), **7z** (LZMA2, smaller),
**tar.gz**, **tar.xz** or **tar.zst**. The Gallery can pack the pictures you pick too.

The work (big archives take a while to list) runs in the background with a progress bar and *Stop*.

Read: zip (with zip64, deflate / bzip2 / lzma / xz / zstd inside), 7z (LZMA, LZMA2, PPMd, BCJ, deflate, bzip2),
**rar 4 and rar 5**, tar (plain, .gz, .bz2, .xz, .zst, .lz), single .gz / .bz2 / .xz / .zst files, iso, cab, lha,
cpio, ar / deb, and comics (cbz, cbr, cb7, cbt). Through [libarchive](https://libarchive.org) with zlib, bzip2,
liblzma (xz) and zstd built in. Not supported: encrypted rar contents, AES-encrypted zip on Linux / Android
(classic zip passwords work everywhere; AES ones on Windows), multi-volume rar.

Unpacking never writes outside the chosen folder: absolute paths, `..` and links in the archive are left out, and
characters that Windows or memory cards don't allow become `_`.

## Comics

*Comics* (next to *Archives* at the top of the tab) shows the **cbz, cbr, cb7 and cbt** files of those folders as
covers (the first page), the ones read lately first, with how far each one is read (a red bar; a tick when
finished). Tapping one unpacks its pages into the cache - the last six comics stay there, so reopening is instant -
and opens them in the picture viewer **at the page where you stopped**: swipe or arrows, pinch / wheel to zoom,
slideshow. The book button reads **right to left** (manga: swipes and arrows go the other way); it is remembered per
comic. Pages are the pictures inside (JPEG, PNG, WebP, GIF...) in natural order (`page2` before `page10`), folders
in order too; macOS leftovers (`__MACOSX`, `._*`) are skipped.

Any archive that is mostly pictures can be read the same way: open it and press **Read**. Opening a comic from the
file manager (*Open with*) opens it straight away. Places are kept in `comic_positions.json`.
