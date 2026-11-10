# Gallery

The **Gallery** tab (on phones: *Gallery* on the home screen or in *Library*) shows the pictures and videos of:

* the camera and *Pictures* (Android: `DCIM` and `Pictures`; desktops: `~/Pictures`),
* the received files folder,
* the folders you add (*Folders... → Add a folder...*). *Videos too* can be switched off there.

Two views:

* **By month** - newest first, a header per month. The date is when the photo was taken (its EXIF
  `DateTimeOriginal`), else the file's date. The dates are read once, in the background, from the first 64 KB of
  each JPEG, and kept in `gallery_dates.json`.
* **Albums** - one tile per folder, with the newest picture on its cover and the number of pictures.

Tapping a picture opens the full-screen viewer with the rest of that list: swipe or arrows, pinch / wheel to zoom,
**Slideshow** (the next one every 4 seconds; a touch or Esc stops it), **Delete**, open with another app, share.
Videos play in the movie player.

**Picking several:** a long press (desktops: Ctrl+click, or *Select*) picks pictures; then *Send...* (to a chat) or
*Delete...* (asks first).

**Back up the camera** (phones, *Back up...*): new photos and videos of `DCIM/Camera` go to one of your devices by
themselves - a [folder sync](ACCOUNTS.md) to that device's download folder, checked every minute and when it comes
online. Nothing is deleted on either side.

Pictures: JPEG (EXIF orientation applied; the small EXIF preview makes the grid fast), PNG, GIF (first frame), BMP,
TGA, PSD, HDR, PNM and **WebP** (libwebp). HEIC isn't supported.
