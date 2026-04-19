# Sleep Screen and Wallpaper System

## Sleep modes

| Mode | Behavior |
|------|----------|
| **Dark** | Solid dark screen |
| **Light** | Solid light screen |
| **Custom** | Show a wallpaper from the `/sleep` folder |
| **Cover** | Show the cover of the last opened book |
| **Blank** | Leave the current screen as-is |
| **Cover + Custom** | Cover if available, otherwise a custom wallpaper |
| **Quotes** | Show a random saved quote |
| **Quotes + Custom** | Quote if available, otherwise a custom wallpaper |
| **Freeze** | Keep whatever was on screen before sleep |

## Cover display

When using Cover or Cover + Custom:

| Setting | Options |
|---------|---------|
| Cover mode | Fit (letterbox) or Crop (fill screen) |
| Cover filter | None, Black & White, Inverted Black & White |

## Custom wallpaper lookup

The firmware looks for a custom BMP in this order:

1. Files inside `/sleep/`
2. `/sleep_F.bmp`
3. `/sleep.bmp`

If none is found, the default DX34 sleep screen is used.

## Playlist behavior

The playlist cap is **200 entries**.

**Up to 200 images:**
- A persisted playlist lives in memory and on SD.
- Default order is stable and filename-based.
- **Randomize Sleep Images** reshuffles the playlist.
- New images added to `/sleep` are pushed near the front so they appear quickly.

**More than 200 images:**
- The firmware avoids storing the full playlist (ESP32-C3 RAM is tight).
- **Randomize Sleep Images** picks a random starting file.
- After that, the device advances sequentially through sorted filenames.

## Favorites

- Favorite files are stored with an `_F` suffix.
- Favorites display with a `[F]` prefix in UI labels.
- Favorites inside `/sleep` are protected from automatic trimming.
- The protected favorite count is capped at 200.

## Trimming and `/sleep pause`

If `/sleep` grows past the cap, non-favorites are automatically moved to `/sleep pause`. This keeps the playlist bounded and memory use predictable.

## Management tools

Settings and the in-book wallpaper triage menu expose:

- Randomize sleep images
- Inspect the last sleep wallpaper
- Pause / unpause wallpaper rotation
- Move the last wallpaper to `/sleep pause`
- Favorite / unfavorite the last wallpaper
- Delete the last wallpaper
- Show the wallpaper filename on screen (toggle)
