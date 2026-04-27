# PXC parity with BMP across firmware

Status: design approved 2026-04-27.
Author: CLU1 / Diogo.

## Goal

Treat `.pxc` (pre-dithered 2 bpp screen-sized image) as a first-class viewable
image format anywhere `.bmp` is currently treated as one: `/sleep` browser,
sleep-pause flow, library browser everywhere, image viewer activity,
favorites, rename / move / delete, and the web `/files` listing.

The `.pxc` wallpaper paths shipped in commit 0facea4 (sleep wallpaper render
and root fallback) are not changed by this work — they continue to operate as
they do today.

## Non-goals

- No support for arbitrary-sized `.pxc`. PXC stays strictly screen-sized; an
  off-size file is a render error, exactly as it is today in the sleep
  renderer.
- No browser-side decode of `.pxc` for the web `/files` page. PXC files are
  listed and downloadable but not previewed.
- No `state.json` migration. The on-disk JSON key `favoriteBmpPaths` stays as
  it is; only C++ symbols are renamed.
- No new translation strings. Existing `STR_FAILED_OPEN_BMP` /
  `STR_INVALID_BMP` are reused for PXC error cases. The string keys keep their
  BMP names; the user-visible text is already generic enough ("Failed to open
  image" / "Invalid image").

## Constraints

1. **Screen-size-only.** PXC layout is `uint16 width` + `uint16 height` +
   packed 2 bpp payload. The renderer rejects any file whose width or height
   differs from the screen by more than 1 px.
2. **EPUB cache files must remain invisible.** The EPUB image renderer writes
   cache files named `<basename>.pxc` and `<basename>_q.pxc` next to the
   source image (see `lib/Epub/Epub/blocks/ImageBlock.cpp`). These are
   internal cache and must not appear in the library browser, the web
   `/files` page, or any favorites flow.
3. **Single source of truth for the PXC renderer.** Sleep, library viewer,
   and any future caller share one renderer.

## Filtering rule for EPUB cache PXC files

Apply identically in the native library browser scan and the web `/files`
listing:

- Always hide `*_q.pxc`.
- Hide `<base>.pxc` if a sibling file in the same directory matches
  `<base>.{jpg,jpeg,png,gif,webp}` (case-insensitive). The presence of a
  book-format source image with the same basename is the signal that the
  `.pxc` is a render cache, not a user image. `.bmp` is intentionally
  **excluded** from the sibling-source list: a `.bmp` and `.pxc` with the
  same basename in `/sleep` are two independent user wallpapers (per the
  Q3 design decision), not a source/cache pair.
- Otherwise show the `.pxc` as a user image.

This preserves the "treat .bmp and .pxc independently" decision for `/sleep`
(where users place their own files and there are no source siblings), while
keeping book folders clean.

## Components

### New: `src/util/PxcRenderer.{h,cpp}`

Free function extracted from `SleepActivity::renderPxcSleepScreen`:

```cpp
namespace PxcRenderer {
// Streams a screen-sized .pxc into the framebuffer using the configured
// grayscale mode (FactoryQuality if SETTINGS.useFactoryLUT, else
// Differential). Returns false if open fails, header read fails, the
// declared dimensions diverge from the screen by more than 1 px, or the row
// buffer cannot be allocated. Does NOT call displayBuffer — caller picks
// the refresh mode.
bool renderPxc(GfxRenderer& renderer, const std::string& path);
}  // namespace PxcRenderer
```

`SleepActivity::renderPxcSleepScreen` becomes a thin wrapper that calls
`PxcRenderer::renderPxc` and then handles its existing optional filename
label + HALF_REFRESH.

### Renamed (C++ symbols only — JSON state untouched)

| Old | New |
|---|---|
| `FavoriteBmp` namespace, `FavoriteBmp.{h,cpp}` | `FavoriteImage` namespace, `FavoriteImage.{h,cpp}` |
| `Mode::BMP_VIEW` | `Mode::IMAGE_VIEW` |
| `MyLibraryActivity::isBmpFile` | `MyLibraryActivity::isImageFile` (= bmp \|\| pxc) |
| `enterBmpView` | `enterImageView` |
| `renderBmpView` | `renderImageView` |
| `bmpViewFullyLoaded` | `imageViewFullyLoaded` |
| `openKeyboardForRenameBmp` | `openKeyboardForRenameImage` |
| `renameSelectedBmp` | `renameSelectedImage` |

`APP_STATE.favoriteBmpPaths` keeps its name — both as the C++ field and the
JSON key — so existing user `state.json` files keep their favorites.
Internally, `FavoriteImage` is documented to operate on both `.bmp` and
`.pxc` despite the legacy field name.

A new helper:

```cpp
bool MyLibraryActivity::isPxcFile(const std::string& filename);  // ".pxc"
bool MyLibraryActivity::isBmpFile(const std::string& filename);  // ".bmp" (kept private — used by isImageFile)
bool MyLibraryActivity::isImageFile(const std::string& filename); // bmp || pxc
```

`isManagedFile` becomes `isBookFile(name) || isImageFile(name)`.

### Extended

#### `FavoriteImage::setFavorite`

Accepts paths whose extension is `.bmp` or `.pxc`. The `_F` favorite suffix
is inserted before the actual extension — i.e. `foo.pxc` becomes
`foo_F.pxc`, `bar.bmp` becomes `bar_F.bmp`. The internal helpers
`hasFavoriteSuffix`, `addFavoriteSuffix`, `stripFavoriteSuffix` operate on
the last 4 characters (which is the same length for both extensions).

The internal `isBmpPathInternal` predicate is renamed to `isImagePath` and
checks for either extension.

`countProtectedSleepFavorites` counts both `.bmp` and `.pxc` favorites in
`/sleep` against the same `SLEEP_FAVORITES_MAX = 500` cap. There is no
per-format cap.

`SetFavoriteResult::NotBmp` is renamed to `SetFavoriteResult::NotImage`.

#### `MyLibraryActivity::renderImageView`

Branches on the actual extension of `selectedFilePath`:

- `.bmp` → existing path unchanged: `Bitmap` parse, optional centering,
  `HALF_REFRESH` BW pass, optional grayscale double pass when present, mark
  `imageViewFullyLoaded`.
- `.pxc` → `PxcRenderer::renderPxc`, then a single `HALF_REFRESH`. No
  centering math (PXC is screen-sized by contract). No grayscale double pass
  (PXC is already pre-dithered with the factory waveform). Mark
  `imageViewFullyLoaded` immediately on success. On failure, render the
  same `STR_INVALID_BMP` / `STR_FAILED_OPEN_BMP` error path as BMP.

#### `MyLibraryActivity::renameSelectedImage`

The previous implementation hard-coded `.bmp` for both extension stripping
and the rebuilt target path. The new implementation:

- Detects the original extension from `selectedFilePath`.
- Strips a user-typed trailing `.bmp` or `.pxc` (case-insensitive) and a
  trailing `_F` from the entered base name.
- Rebuilds the target path with the same original extension.

The auto-suffix collision loop and the reload-and-snap-cursor logic are
unchanged.

#### Library browser folder scan

After populating `files` for the current folder, apply the EPUB cache
filter described above. Implementation: build a `std::unordered_set<string>`
of source-image basenames in the listing once per folder load, then drop
any `_q.pxc` and any `.pxc` whose basename appears in the source-image
set. O(n) over the listing, no extra SD I/O.

#### Sleep playlist / sleep-pause

`SdFatSleepFs::isSleepImageExt` already accepts both `.bmp` and `.pxc`, and
`SleepActivity::onResume` already re-renders the saved
`lastSleepWallpaperPath` via PXC when the path ends in `.pxc`
(SleepActivity.cpp:150). No code changes needed; verification only.

`syncSleepPlaylistWithFiles` and `randomizeSleepImagePlaylist` already
operate on whatever `getValidSleepBitmaps` returns, which uses the
extension predicate. No changes needed.

#### Web `/files`

In `CrossPointWebServer.cpp`:

- Add `info.isPxc` populated the same way as `info.isBmp`.
- Apply the EPUB cache filter to listings: same `_q.pxc` / sibling-source
  rule.
- Existing BMP download endpoint pattern reused for PXC; MIME type
  `application/octet-stream` (browser cannot render PXC anyway).

In `FilesPage.html`:

- Add a `pxc-file` row class mirroring `bmp-file`.
- Add a `PXC` badge mirroring `BMP`.
- No browser preview path. The PXC entry is downloadable like any other
  file.

The hashed CSS index is regenerated as part of the build.

## Data flow — library viewer for .pxc

1. User selects `foo.pxc` in the library browser.
2. `enterImageView(path)` sets `selectedFilePath`, `Mode::IMAGE_VIEW`,
   `imageViewFullyLoaded = false`, queues a clean refresh.
3. `renderImageView()` detects `.pxc` and calls
   `PxcRenderer::renderPxc(renderer, selectedFilePath)`.
4. On success: button hints drawn, `HalDisplay::HALF_REFRESH`,
   `imageViewFullyLoaded = true`.
5. On open fail / header fail / size mismatch: error text via
   `STR_FAILED_OPEN_BMP` or `STR_INVALID_BMP`, back-button hint, frame
   displayed.
6. Action menu (rename / favorite / move / delete) operates on
   `selectedFilePath` regardless of extension.

## Heap impact

- `PxcRenderer::renderPxc` allocates one row buffer of `(screenWidth + 3) /
  4` bytes (≈75 B on a 296-px screen) for the duration of the render and
  frees it before returning. No persistent heap commitment.
- The folder-scan EPUB-cache filter builds a transient
  `unordered_set<string>` of source-image basenames per folder load. Bounded
  by the existing browser cap (300 normally, 1000 for `/sleep`).

## Testing

Manual on device:

- Drop a screen-sized `.pxc` into `/sleep`, `/`, `/Books`, `/Wallpapers`.
  Verify each appears in the library browser, opens via the image viewer,
  favorites correctly (renames to `*_F.pxc`), renames via keyboard,
  deletes.
- Drop an EPUB book whose images get cached as `.pxc` and `_q.pxc`
  siblings. Open the book to populate the cache, return to the library
  browser, verify the cache files are invisible.
- Drop a standalone `_q.pxc` anywhere outside `/sleep`. Verify it is
  hidden.
- Enter sleep mode with a `.pxc` selected from the playlist; pause sleep
  (button press); resume; verify the same `.pxc` re-renders.
- Open the web `/files` page; verify each `.pxc` shows with a `PXC` badge,
  is not previewed, and is downloadable. Verify no `_q.pxc` or
  source-shadowed `.pxc` appears.
- Boot with an existing `state.json` containing `favoriteBmpPaths`; verify
  favorites are preserved and continue to render the `[F]` prefix.
- Rename / move / delete on `.pxc` favorites; verify
  `APP_STATE.favoriteBmpPaths` updates and `state.json` round-trips.
- Drop a `.pxc` whose declared dimensions do not match the screen; verify
  the viewer shows the invalid-image error rather than rendering garbage.

Build:

- `pio run -e default` — confirm Flash% / RAM% deltas.
- `pio run -e debug -t upload` for serial verification of the cache filter
  log lines and PXC render log lines.
