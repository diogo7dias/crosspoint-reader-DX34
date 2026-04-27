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

The firmware looks for a custom wallpaper in this order:

1. Files inside `/sleep/` (`.bmp` or `.pxc`)
2. `/sleep.pxc` at the SD root (preferred)
3. `/sleep_F.bmp` at the SD root
4. `/sleep.bmp` at the SD root

If none is found, the default DX34 sleep screen is used.

## Wallpaper formats: `.bmp` vs `.pxc`

DX34 reads two wallpaper file types. They share the rotation, the favorites system, and the playlist — pick whichever fits the source image.

### `.bmp` (legacy, on-device dithered)

A standard BMP image. The firmware reads pixels from the file at sleep time and runs a dithering pass on the device to quantise them down to the panel's four grey levels. Works with anything you can save as a BMP. The dithering is decent but bound by what fits in the device's tight CPU/heap budget.

### `.pxc` (recommended, pre-dithered)

PXC is a small native format storing a pre-dithered 2-bit (4-level grey) image with embedded dimensions, packed at 4 pixels per byte. The dithering is done **once, in the browser**, with full precision. The device then reads the file straight into the panel — no decode, no dithering, no quantisation noise.

**Why PXC is superior:**

- **Better image quality.** Dithering happens in the converter with full floating-point precision and access to algorithms (Floyd-Steinberg, etc.) that don't fit on the device's runtime budget. The result is bound by the converter, not by what the firmware can squeeze in.
- **Faster sleep wake.** The on-device dither pass is skipped entirely — the file is already in the format the panel wants.
- **Smaller files.** Roughly 6× smaller than the equivalent BMP at the same resolution. More wallpapers fit on your SD card and the V2 playlist scans them faster.
- **Cleaner gradients.** No quantisation artefacts from runtime dithering — the converter picks the best 4-level approximation per pixel and writes it directly.
- **Ideal pair with Factory Grayscale.** When **Display → Factory Grayscale** is enabled, PXC files render through the panel's manufacturer waveform (`lut_factory_quality`) for the broadest tonal range. With the toggle off they still render correctly via the differential path — just less sharp.

If you already use a converter to resize an image to panel dimensions before dropping it in `/sleep/`, going straight to PXC is the logical next step: skip the lossy on-device dither stage and hand the panel exactly what it expects.

### How to convert images to `.pxc`

Use the official converter: **<https://crosspoint-pxc-converter.pages.dev/>**

1. Open the converter in a browser.
2. Upload an image at panel resolution (480 × 800 portrait).
3. Pick a dither algorithm.
4. Download the resulting `.pxc`.

### How to put files on the device

The X4 firmware does **not** present as a USB drive — only a USB serial port. File transfer goes through the device's built-in web server:

1. **Settings → Wi-Fi networks** on the device. Connect to the same Wi-Fi as your computer.
2. Note the device's IP address shown on the Wi-Fi screen.
3. On your computer, open `http://<device-ip>/files` in a browser.
4. Upload the `.pxc` (or `.bmp`) file. Place inside `/sleep/` for the rotation, or as `/sleep.pxc` at the root for the static fallback.
5. Trigger sleep — the new wallpaper appears.

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
