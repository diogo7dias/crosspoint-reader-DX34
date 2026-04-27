# PXC Image Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Treat `.pxc` (pre-dithered 2 bpp screen-sized image) as a first-class viewable image format anywhere `.bmp` is treated as one today: library browser, image viewer, favorites, rename/move/delete, sleep flows, web `/files`.

**Architecture:** Extract the existing `SleepActivity::renderPxcSleepScreen` body into a shared `PxcRenderer` free function so both sleep and the library viewer share one implementation. Rename the C++ `FavoriteBmp` module to `FavoriteImage` and broaden its predicates to cover `.pxc`. Branch the library viewer on extension. Filter EPUB-render-cache `.pxc` files from the library browser and the web `/files` page.

**Tech Stack:** C++ on PlatformIO/Arduino/ESP32-C3, Unity for host-side tests via `pio test -e test_host`, vanilla HTML/JS for the web UI in `src/network/html/FilesPage.html`.

**Spec:** [docs/superpowers/specs/2026-04-27-pxc-image-parity-design.md](../specs/2026-04-27-pxc-image-parity-design.md)

---

## File map

**Created:**
- `src/util/PxcRenderer.h` — public header for the shared PXC renderer
- `src/util/PxcRenderer.cpp` — implementation extracted from `SleepActivity`
- `src/util/FavoriteImage.h` — renamed module header
- `src/util/FavoriteImage.cpp` — renamed module impl, extended for `.pxc`
- `test/test_favorite_image/test_main.cpp` — host-side Unity tests for the suffix helpers and the cache-filter helper

**Modified:**
- `src/activities/boot_sleep/SleepActivity.cpp` — `renderPxcSleepScreen` becomes a thin wrapper around `PxcRenderer::renderPxc`
- `src/activities/home/MyLibraryActivity.h` — mode rename, viewer state rename, new helpers, cache-filter helper
- `src/activities/home/MyLibraryActivity.cpp` — branch viewer on extension, use `FavoriteImage`, apply cache filter, rename method
- `src/main.cpp`, `src/sleep/WallpaperPlaylist.h`, `src/network/CrossPointWebServer.{h,cpp}`, `src/components/themes/BaseTheme.cpp`, `src/activities/reader/EpubReaderMenuActivity.cpp`, `src/activities/reader/EpubReaderActivity.cpp`, `src/activities/home/HomeActivity.cpp` — include path + namespace updates
- `src/network/CrossPointWebServer.cpp` — add `info.isPxc`, apply cache filter to listings, accept `.pxc` in download/upload allowlists where applicable
- `src/network/html/FilesPage.html` — add `pxc-file` row class, `PXC` badge

**Deleted:**
- `src/util/FavoriteBmp.h`, `src/util/FavoriteBmp.cpp` (replaced by `FavoriteImage.{h,cpp}`)

---

## Task 1: Add `isPxcFile` and `isImageFile` predicates to `MyLibraryActivity`

**Files:**
- Modify: `src/activities/home/MyLibraryActivity.h`
- Modify: `src/activities/home/MyLibraryActivity.cpp`

Tiny, additive step — leaves all existing `isBmpFile` callers compiling. Used by later tasks.

- [ ] **Step 1: Add the new declarations to the header.**

In `src/activities/home/MyLibraryActivity.h`, locate the existing `static bool isBmpFile(const std::string& filename);` declaration and add two siblings immediately below it:

```cpp
  static bool isBmpFile(const std::string& filename);
  static bool isPxcFile(const std::string& filename);
  static bool isImageFile(const std::string& filename);
```

- [ ] **Step 2: Implement them in the .cpp.**

In `src/activities/home/MyLibraryActivity.cpp`, find the existing `isBmpFile` definition (~line 414) and add the two new helpers immediately below:

```cpp
bool MyLibraryActivity::isBmpFile(const std::string& filename) {
  return StringUtils::checkFileExtension(filename, ".bmp");
}

bool MyLibraryActivity::isPxcFile(const std::string& filename) {
  return StringUtils::checkFileExtension(filename, ".pxc");
}

bool MyLibraryActivity::isImageFile(const std::string& filename) {
  return isBmpFile(filename) || isPxcFile(filename);
}
```

- [ ] **Step 3: Build to confirm no regressions.**

Run: `pio run -e default`
Expected: build succeeds, no new warnings.

- [ ] **Step 4: Commit.**

```bash
git add src/activities/home/MyLibraryActivity.h src/activities/home/MyLibraryActivity.cpp
git commit -m "feat(library): add isPxcFile/isImageFile predicates"
```

---

## Task 2: Extract `PxcRenderer` from `SleepActivity`

**Files:**
- Create: `src/util/PxcRenderer.h`
- Create: `src/util/PxcRenderer.cpp`
- Modify: `src/activities/boot_sleep/SleepActivity.cpp`

The current implementation lives at `src/activities/boot_sleep/SleepActivity.cpp:477-534`. Move the body into a free function; keep the existing wrapper method calling into it so the sleep filename label and the optional final HALF_REFRESH stay where they are.

- [ ] **Step 1: Create the header.**

Write `src/util/PxcRenderer.h`:

```cpp
#pragma once

#include <string>

class GfxRenderer;

namespace PxcRenderer {

// Streams a screen-sized .pxc into the framebuffer using the configured
// grayscale mode (FactoryQuality if SETTINGS.useFactoryLUT, else
// Differential). Returns false if the file cannot be opened, the header
// cannot be read, the declared dimensions diverge from the current screen
// by more than 1 px, or the per-row buffer cannot be allocated.
//
// Does NOT call displayBuffer — caller picks the refresh mode after this
// returns.
//
// PXC layout: uint16_t width, uint16_t height, then packed 2 bpp payload
// (4 px/byte, MSB first). Pixel convention: 0=Black, 1=DarkGray,
// 2=LightGray, 3=White (matches Bitmap::readNextRow).
bool renderPxc(GfxRenderer& renderer, const std::string& path);

}  // namespace PxcRenderer
```

- [ ] **Step 2: Create the implementation.**

Write `src/util/PxcRenderer.cpp` by lifting lines 477-527 of `SleepActivity.cpp` (everything from the file open through `free(rowBuf)` / `file.close()`), wrapped in the new namespace:

```cpp
#include "PxcRenderer.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include <cstdint>
#include <cstdlib>

#include "../CrossPointSettings.h"
#include "../components/converters/DirectPixelWriter.h"

namespace PxcRenderer {

bool renderPxc(GfxRenderer& renderer, const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("PXC", path, file)) {
    LOG_ERR("PXC", "Cannot open: %s", path.c_str());
    return false;
  }
  uint16_t pxcWidth = 0, pxcHeight = 0;
  if (file.read(&pxcWidth, 2) != 2 || file.read(&pxcHeight, 2) != 2) {
    LOG_ERR("PXC", "Header read failed: %s", path.c_str());
    file.close();
    return false;
  }
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  if (std::abs(static_cast<int>(pxcWidth) - sw) > 1 ||
      std::abs(static_cast<int>(pxcHeight) - sh) > 1) {
    LOG_ERR("PXC", "Size mismatch %dx%d (screen %dx%d): %s",
            pxcWidth, pxcHeight, sw, sh, path.c_str());
    file.close();
    return false;
  }
  const uint32_t dataOffset = file.position();
  const int bytesPerRow = (pxcWidth + 3) / 4;
  uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuf) {
    LOG_ERR("PXC", "Row alloc failed (%d bytes)", bytesPerRow);
    file.close();
    return false;
  }

  const auto mode = SETTINGS.useFactoryLUT
                        ? GfxRenderer::GrayscaleMode::FactoryQuality
                        : GfxRenderer::GrayscaleMode::Differential;
  const int width = static_cast<int>(pxcWidth);
  const int height = static_cast<int>(pxcHeight);
  renderer.renderGrayscale(mode, [&]() {
    file.seek(dataOffset);
    DirectPixelWriter pw;
    pw.init(renderer);
    for (int row = 0; row < height; row++) {
      if (file.read(rowBuf, bytesPerRow) != bytesPerRow) break;
      pw.beginRow(row);
      for (int col = 0; col < width; col++) {
        const uint8_t pv = (rowBuf[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;
        pw.writePixel(col, pv);
      }
      if ((row & 31) == 0) esp_task_wdt_reset();
    }
  });

  free(rowBuf);
  file.close();
  return true;
}

}  // namespace PxcRenderer
```

Verify the include path for `DirectPixelWriter` matches what `SleepActivity.cpp` currently uses (open the existing file and copy its include line if the relative path above is wrong).

- [ ] **Step 3: Replace the body of `SleepActivity::renderPxcSleepScreen`.**

In `src/activities/boot_sleep/SleepActivity.cpp`, replace the body of `renderPxcSleepScreen` (the entire function at lines 477-534) with this thin wrapper that retains the optional filename label and final HALF_REFRESH:

```cpp
bool SleepActivity::renderPxcSleepScreen(const std::string& path, const char* sourceFilename) const {
  if (!PxcRenderer::renderPxc(renderer, path)) {
    return false;
  }

  if (sourceFilename != nullptr && SETTINGS.showSleepImageFilename) {
    drawSleepFilenameLabel(renderer, sourceFilename);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
  return true;
}
```

Add `#include "../../util/PxcRenderer.h"` at the top of `SleepActivity.cpp` next to the existing util/ includes. Remove now-unused includes (e.g. `DirectPixelWriter.h`) only if they are not referenced by the rest of the file — search before deleting.

- [ ] **Step 4: Build.**

Run: `pio run -e default`
Expected: build succeeds. `firmware.bin` size should change negligibly.

- [ ] **Step 5: Commit.**

```bash
git add src/util/PxcRenderer.h src/util/PxcRenderer.cpp src/activities/boot_sleep/SleepActivity.cpp
git commit -m "refactor(pxc): extract renderPxc into shared PxcRenderer util"
```

---

## Task 3: Rename `FavoriteBmp` → `FavoriteImage` (mechanical), keep BMP-only behavior

**Files:**
- Create: `src/util/FavoriteImage.h`
- Create: `src/util/FavoriteImage.cpp`
- Delete: `src/util/FavoriteBmp.h`, `src/util/FavoriteBmp.cpp`
- Modify: every caller listed under "File map → Modified"

This task is a pure rename — no semantic changes. The next task adds `.pxc` support inside `FavoriteImage`. Splitting the rename from the behavior change keeps the diff reviewable.

- [ ] **Step 1: `git mv` the files.**

```bash
git mv src/util/FavoriteBmp.h src/util/FavoriteImage.h
git mv src/util/FavoriteBmp.cpp src/util/FavoriteImage.cpp
```

- [ ] **Step 2: Rename the namespace and the `NotBmp` enum value inside the new files.**

In both `src/util/FavoriteImage.h` and `src/util/FavoriteImage.cpp`:

- Replace `namespace FavoriteBmp` with `namespace FavoriteImage`.
- Replace `}  // namespace FavoriteBmp` with `}  // namespace FavoriteImage`.
- Replace `SetFavoriteResult::NotBmp` with `SetFavoriteResult::NotImage` (declaration in the header, every reference in the .cpp).
- Update the `#include "FavoriteBmp.h"` line at the top of `FavoriteImage.cpp` to `#include "FavoriteImage.h"`.

Internal helpers like `isBmpPathInternal` keep their names and behavior in this task.

- [ ] **Step 3: Update every caller.**

For each file listed below, update the include and the namespace references via search-and-replace:

```bash
git grep -l 'FavoriteBmp' -- 'src/' 'lib/'
```

In each result:
- `#include "util/FavoriteBmp.h"` or `#include "FavoriteBmp.h"` → corresponding `FavoriteImage.h` path.
- `FavoriteBmp::` → `FavoriteImage::`.
- Any `SetFavoriteResult::NotBmp` → `SetFavoriteResult::NotImage`.

Expected file list (from `git grep`): `src/main.cpp`, `src/sleep/WallpaperPlaylist.h`, `src/network/CrossPointWebServer.{h,cpp}`, `src/components/themes/BaseTheme.cpp`, `src/activities/reader/EpubReaderMenuActivity.cpp`, `src/activities/reader/EpubReaderActivity.cpp`, `src/activities/home/MyLibraryActivity.{h,cpp}`, `src/activities/home/HomeActivity.cpp`, `src/activities/boot_sleep/SleepActivity.cpp`.

- [ ] **Step 4: Confirm no stragglers.**

Run: `git grep -n 'FavoriteBmp\|NotBmp'`
Expected: zero results.

- [ ] **Step 5: Build.**

Run: `pio run -e default`
Expected: build succeeds.

- [ ] **Step 6: Commit.**

```bash
git add -A
git commit -m "refactor(favorites): rename FavoriteBmp module to FavoriteImage"
```

---

## Task 4: Extend `FavoriteImage` to handle `.pxc`

**Files:**
- Modify: `src/util/FavoriteImage.cpp`

Internal predicate `isBmpPathInternal` currently checks only `.bmp`. Generalize it to accept `.pxc` as well, and rename it for clarity. The `_F` suffix logic already operates on the last 4 characters, which is correct for both extensions.

- [ ] **Step 1: Replace `isBmpPathInternal` with `isImagePath`.**

In `src/util/FavoriteImage.cpp`, locate (around line 17):

```cpp
bool isBmpPathInternal(const std::string& path) { return StringUtils::checkFileExtension(path, ".bmp"); }
```

Replace with:

```cpp
bool isImagePath(const std::string& path) {
  return StringUtils::checkFileExtension(path, ".bmp") ||
         StringUtils::checkFileExtension(path, ".pxc");
}
```

- [ ] **Step 2: Rename every internal call site.**

In the same file, replace all `isBmpPathInternal(` with `isImagePath(`. Verify with:

```bash
grep -n 'isBmpPathInternal\|isImagePath' src/util/FavoriteImage.cpp
```

Expected: no `isBmpPathInternal` remains; one definition + multiple call sites of `isImagePath`.

- [ ] **Step 3: Build to confirm no callers were missed.**

Run: `pio run -e default`
Expected: build succeeds.

- [ ] **Step 4: Commit.**

```bash
git add src/util/FavoriteImage.cpp
git commit -m "feat(favorites): accept .pxc in FavoriteImage predicates"
```

---

## Task 5: Host-side tests for `FavoriteImage` suffix helpers

**Files:**
- Create: `test/test_favorite_image/test_main.cpp`

The pure helpers `hasFavoriteSuffix`, `addFavoriteSuffix`, `stripFavoriteSuffix` are exposed in the public header and don't depend on `Storage` or `APP_STATE`, so they're testable on the host. We test BMP behavior preservation and the new PXC behavior.

- [ ] **Step 1: Write the failing test file.**

Write `test/test_favorite_image/test_main.cpp`:

```cpp
// Host-side tests for FavoriteImage suffix helpers.
// Run via: pio test -e test_host -f test_favorite_image

#include <unity.h>

#include <string>

#include "util/FavoriteImage.h"

void setUp() {}
void tearDown() {}

namespace {

void test_has_favorite_suffix_bmp() {
  TEST_ASSERT_TRUE(FavoriteImage::hasFavoriteSuffix("foo_F.bmp"));
  TEST_ASSERT_FALSE(FavoriteImage::hasFavoriteSuffix("foo.bmp"));
  TEST_ASSERT_FALSE(FavoriteImage::hasFavoriteSuffix("F.bmp"));
}

void test_has_favorite_suffix_pxc() {
  TEST_ASSERT_TRUE(FavoriteImage::hasFavoriteSuffix("foo_F.pxc"));
  TEST_ASSERT_FALSE(FavoriteImage::hasFavoriteSuffix("foo.pxc"));
  TEST_ASSERT_FALSE(FavoriteImage::hasFavoriteSuffix("F.pxc"));
}

void test_has_favorite_suffix_rejects_non_image() {
  TEST_ASSERT_FALSE(FavoriteImage::hasFavoriteSuffix("foo_F.txt"));
  TEST_ASSERT_FALSE(FavoriteImage::hasFavoriteSuffix("foo_F.epub"));
}

void test_add_favorite_suffix_bmp() {
  TEST_ASSERT_EQUAL_STRING("foo_F.bmp", FavoriteImage::addFavoriteSuffix("foo.bmp").c_str());
  TEST_ASSERT_EQUAL_STRING("foo_F.bmp", FavoriteImage::addFavoriteSuffix("foo_F.bmp").c_str());
}

void test_add_favorite_suffix_pxc() {
  TEST_ASSERT_EQUAL_STRING("foo_F.pxc", FavoriteImage::addFavoriteSuffix("foo.pxc").c_str());
  TEST_ASSERT_EQUAL_STRING("foo_F.pxc", FavoriteImage::addFavoriteSuffix("foo_F.pxc").c_str());
}

void test_add_favorite_suffix_passthrough_for_non_image() {
  TEST_ASSERT_EQUAL_STRING("foo.txt", FavoriteImage::addFavoriteSuffix("foo.txt").c_str());
}

void test_strip_favorite_suffix_bmp() {
  TEST_ASSERT_EQUAL_STRING("foo.bmp", FavoriteImage::stripFavoriteSuffix("foo_F.bmp").c_str());
  TEST_ASSERT_EQUAL_STRING("foo.bmp", FavoriteImage::stripFavoriteSuffix("foo.bmp").c_str());
}

void test_strip_favorite_suffix_pxc() {
  TEST_ASSERT_EQUAL_STRING("foo.pxc", FavoriteImage::stripFavoriteSuffix("foo_F.pxc").c_str());
  TEST_ASSERT_EQUAL_STRING("foo.pxc", FavoriteImage::stripFavoriteSuffix("foo.pxc").c_str());
}

}  // namespace

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_has_favorite_suffix_bmp);
  RUN_TEST(test_has_favorite_suffix_pxc);
  RUN_TEST(test_has_favorite_suffix_rejects_non_image);
  RUN_TEST(test_add_favorite_suffix_bmp);
  RUN_TEST(test_add_favorite_suffix_pxc);
  RUN_TEST(test_add_favorite_suffix_passthrough_for_non_image);
  RUN_TEST(test_strip_favorite_suffix_bmp);
  RUN_TEST(test_strip_favorite_suffix_pxc);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the tests.**

Run: `pio test -e test_host -f test_favorite_image`
Expected: all 8 tests pass. If `FavoriteImage::isFavoritePath` or other helpers transitively pull in `APP_STATE` via the linker even when unused, restrict the test to call only the three pure helpers above; they're declared `extern` and don't reach `APP_STATE`.

If a link error names `APP_STATE` or `Storage`, the most likely cause is that `FavoriteImage.cpp` is being compiled into the test binary. Check `platformio.ini`'s `[env:test_host]` and the `test_filter` / `lib_compat_mode` settings; you may need to mark the test source with `test_build_src = false` for this single env, or add `build_src_filter` to exclude SDK-dependent units. Refer to the existing `test_wallpaper_playlist_v2` test for a working pattern.

- [ ] **Step 3: Commit.**

```bash
git add test/test_favorite_image/
git commit -m "test(favorites): host-side coverage for .bmp and .pxc suffix helpers"
```

---

## Task 6: Rename `Mode::BMP_VIEW` and viewer state symbols in `MyLibraryActivity`

**Files:**
- Modify: `src/activities/home/MyLibraryActivity.h`
- Modify: `src/activities/home/MyLibraryActivity.cpp`

Mechanical rename. Behavior unchanged.

- [ ] **Step 1: Rename the enum value and the state field in the header.**

In `src/activities/home/MyLibraryActivity.h`:

- `Mode::BMP_VIEW` → `Mode::IMAGE_VIEW`.
- `bool bmpViewFullyLoaded` → `bool imageViewFullyLoaded`.
- `void enterBmpView(const std::string& bmpPath)` → `void enterImageView(const std::string& imagePath)`.
- `void renderBmpView()` → `void renderImageView()`.
- `void openKeyboardForRenameBmp()` → `void openKeyboardForRenameImage()`.
- `void renameSelectedBmp(const std::string& newBase)` → `void renameSelectedImage(const std::string& newBase)`.

- [ ] **Step 2: Rename every reference in the .cpp.**

In `src/activities/home/MyLibraryActivity.cpp`, search-and-replace each old symbol to its new name. After the edits, verify:

```bash
git grep -n 'BMP_VIEW\|bmpViewFullyLoaded\|enterBmpView\|renderBmpView\|openKeyboardForRenameBmp\|renameSelectedBmp'
```

Expected: zero results.

- [ ] **Step 3: Build.**

Run: `pio run -e default`
Expected: build succeeds.

- [ ] **Step 4: Commit.**

```bash
git add src/activities/home/MyLibraryActivity.h src/activities/home/MyLibraryActivity.cpp
git commit -m "refactor(library): rename BMP_VIEW symbols to IMAGE_VIEW"
```

---

## Task 7: Switch library callers from `isBmpFile` to `isImageFile`

**Files:**
- Modify: `src/activities/home/MyLibraryActivity.cpp`

Every existing site that gates on "is this a viewable image" should accept `.pxc` too. The only sites to keep as `isBmpFile` are explicit BMP-format-only branches, of which there should be none in this file.

- [ ] **Step 1: Audit current usages.**

Run:

```bash
grep -n 'isBmpFile' src/activities/home/MyLibraryActivity.cpp
```

For each hit, decide:
- If it's "should this row be treated as a viewable image?" → switch to `isImageFile`.
- If it's "is this exactly a BMP, where the BMP-specific code path matters?" → keep as `isBmpFile` and document why with a one-line comment.

Concrete sites you should switch (based on the current source):
- `isManagedFile` (around line 419): `return isBookFile(filename) || isImageFile(filename);`
- `getDisplayNameForRawFile` (around line 432): branch on `isImageFile`, not `isBmpFile`, so PXC favorites get the `[F]` prefix.

- [ ] **Step 2: Build.**

Run: `pio run -e default`
Expected: build succeeds.

- [ ] **Step 3: Commit.**

```bash
git add src/activities/home/MyLibraryActivity.cpp
git commit -m "feat(library): treat .pxc rows as images for display + actions"
```

---

## Task 8: Branch `renderImageView` on extension; add the PXC fast path

**Files:**
- Modify: `src/activities/home/MyLibraryActivity.cpp`

The existing `renderImageView` (formerly `renderBmpView`) is the BMP renderer. Wrap it in an extension branch.

- [ ] **Step 1: Add `#include "../../util/PxcRenderer.h"` near the existing util/ includes** (top of the file).

- [ ] **Step 2: Replace `renderImageView` with a branching version.**

Find the existing `void MyLibraryActivity::renderImageView()` (formerly at ~line 1450) and replace its body with:

```cpp
void MyLibraryActivity::renderImageView() {
  if (isPxcFile(selectedFilePath)) {
    renderPxcImageView();
    return;
  }
  renderBmpImageView();
}
```

Then rename the existing render-the-BMP body to `renderBmpImageView` (declare it `private` in the header beside `renderImageView`) — leave its implementation untouched apart from the rename.

- [ ] **Step 3: Add `renderPxcImageView`.**

Below `renderBmpImageView`, add:

```cpp
void MyLibraryActivity::renderPxcImageView() {
  renderer.clearScreen();

  if (!PxcRenderer::renderPxc(renderer, selectedFilePath)) {
    renderer.drawCenteredText(UI_12_FONT_ID, 80, tr(STR_INVALID_BMP));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    displayFrame();
    imageViewFullyLoaded = true;
    return;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ACTIONS_BUTTON), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  nextRefreshMode = HalDisplay::FAST_REFRESH;
  imageViewFullyLoaded = true;
}
```

- [ ] **Step 4: Declare the two new privates in the header.**

In `src/activities/home/MyLibraryActivity.h`, beside the existing `renderImageView` declaration:

```cpp
  void renderImageView();
  void renderBmpImageView();
  void renderPxcImageView();
```

- [ ] **Step 5: Build.**

Run: `pio run -e default`
Expected: build succeeds.

- [ ] **Step 6: Commit.**

```bash
git add src/activities/home/MyLibraryActivity.h src/activities/home/MyLibraryActivity.cpp
git commit -m "feat(library): PXC fast-path render in image viewer"
```

---

## Task 9: Make `renameSelectedImage` extension-aware

**Files:**
- Modify: `src/activities/home/MyLibraryActivity.cpp`

The current implementation hard-codes `.bmp` for both stripping and rebuilding. Switch to using whatever extension the selected file actually has.

- [ ] **Step 1: Replace the extension handling block.**

In `renameSelectedImage` (formerly `renameSelectedBmp`, ~line 524), find the lines:

```cpp
  // Strip user-typed trailing .bmp (any case) and _F — we control both.
  if (base.size() >= 4) {
    std::string tail = base.substr(base.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(), ::tolower);
    if (tail == ".bmp") base = base.substr(0, base.size() - 4);
  }
```

Replace with:

```cpp
  // Capture the original extension up-front so the rebuilt path keeps it.
  std::string originalExt = ".bmp";
  if (selectedFilePath.size() >= 4) {
    std::string tail = selectedFilePath.substr(selectedFilePath.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(), ::tolower);
    if (tail == ".pxc") originalExt = ".pxc";
  }

  // Strip a user-typed trailing .bmp/.pxc (any case) and _F — we control both.
  if (base.size() >= 4) {
    std::string tail = base.substr(base.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(), ::tolower);
    if (tail == ".bmp" || tail == ".pxc") base = base.substr(0, base.size() - 4);
  }
```

Then, in the same function, find:

```cpp
  const std::string suffix = std::string(wasFav ? "_F" : "") + ".bmp";
```

Replace with:

```cpp
  const std::string suffix = std::string(wasFav ? "_F" : "") + originalExt;
```

- [ ] **Step 2: Verify no other `.bmp` literals slipped through in this function.**

```bash
grep -n '\.bmp' src/activities/home/MyLibraryActivity.cpp
```

For any hit inside `renameSelectedImage`, check whether it should be `originalExt`. Other functions may legitimately keep `.bmp` literals (e.g. defaults in unrelated paths) — only the rename function is in scope here.

- [ ] **Step 3: Build.**

Run: `pio run -e default`
Expected: build succeeds.

- [ ] **Step 4: Commit.**

```bash
git add src/activities/home/MyLibraryActivity.cpp
git commit -m "feat(library): preserve original extension on image rename"
```

---

## Task 10: Add EPUB-cache filter to library browser scan

**Files:**
- Modify: `src/activities/home/MyLibraryActivity.h`
- Modify: `src/activities/home/MyLibraryActivity.cpp`

Filter the populated `files` vector after scan to drop:
- Any `*_q.pxc`.
- Any `*.pxc` whose basename has a sibling `.{jpg,jpeg,png,gif,webp}` in the same listing (case-insensitive). `.bmp` is **excluded** from the sibling-source set per the spec.

- [ ] **Step 1: Add the filter helper to the header.**

In `src/activities/home/MyLibraryActivity.h` (private section):

```cpp
  static void filterEpubCachePxc(std::vector<std::string>& files);
```

- [ ] **Step 2: Implement the helper.**

Place near the other static helpers in `MyLibraryActivity.cpp`:

```cpp
void MyLibraryActivity::filterEpubCachePxc(std::vector<std::string>& files) {
  // Build set of source-image basenames (sans extension, lowercased) present
  // in the listing. .bmp is intentionally excluded — a .bmp + .pxc with the
  // same basename in /sleep are two independent user wallpapers.
  static const char* const kSourceExts[] = {".jpg", ".jpeg", ".png", ".gif", ".webp"};

  auto lowerExt = [](const std::string& name, size_t extLen) {
    if (name.size() < extLen) return std::string{};
    std::string ext = name.substr(name.size() - extLen);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
  };

  auto stemOf = [](const std::string& name) {
    const auto dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
  };

  auto toLower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
  };

  std::unordered_set<std::string> sourceStems;
  for (const auto& name : files) {
    if (name.empty() || name.back() == '/') continue;
    for (const char* ext : kSourceExts) {
      const std::string e = ext;
      if (name.size() >= e.size() && lowerExt(name, e.size()) == e) {
        sourceStems.insert(toLower(stemOf(name)));
        break;
      }
    }
  }

  files.erase(
      std::remove_if(files.begin(), files.end(),
                     [&](const std::string& name) {
                       if (name.empty() || name.back() == '/') return false;
                       if (lowerExt(name, 4) != ".pxc") return false;
                       const std::string lower = toLower(name);
                       // Drop any *_q.pxc.
                       if (lower.size() >= 6 &&
                           lower.compare(lower.size() - 6, 6, "_q.pxc") == 0) {
                         return true;
                       }
                       // Drop *.pxc shadowed by a source-format sibling.
                       return sourceStems.count(toLower(stemOf(name))) > 0;
                     }),
      files.end());
}
```

Add `#include <unordered_set>` and `#include <algorithm>` at the top of the .cpp if not already present.

- [ ] **Step 3: Call the filter at the end of the scan.**

Locate `MyLibraryActivity::loadFiles` (this is where the `files` vector is populated and `orderSleepFolderByPlaylist` is called per the existing memory note). Immediately before the existing `orderSleepFolderByPlaylist`-related logic, call:

```cpp
  filterEpubCachePxc(files);
```

The exact line will be obvious from the scan: it's after the directory iteration completes and before any sorting/ordering.

- [ ] **Step 4: Build.**

Run: `pio run -e default`
Expected: build succeeds.

- [ ] **Step 5: Add a host-side test for the filter.**

Append to `test/test_favorite_image/test_main.cpp` (or create a sibling file `test/test_library_filter/test_main.cpp` if you prefer to keep tests focused — sibling file is recommended):

Create `test/test_library_filter/test_main.cpp`:

```cpp
// Host-side tests for the EPUB-cache PXC filter on library listings.
// Run via: pio test -e test_host -f test_library_filter

#include <unity.h>

#include <string>
#include <vector>

#include "activities/home/MyLibraryActivity.h"

void setUp() {}
void tearDown() {}

namespace {

std::vector<std::string> sampleListing() {
  return {
      // User PXC + BMP wallpapers in /sleep — both must survive.
      "wallpaper1.pxc",
      "wallpaper1.bmp",
      // Standalone PXC with no sibling source — must survive.
      "lonely.pxc",
      // EPUB cache: image1.pxc shadowed by image1.jpg.
      "image1.jpg",
      "image1.pxc",
      // EPUB cache quality variant — always dropped.
      "anything_q.pxc",
      // Non-image files unaffected.
      "book.epub",
      "notes.txt",
      "subdir/",
  };
}

void test_filter_drops_q_pxc() {
  std::vector<std::string> files = sampleListing();
  MyLibraryActivity::filterEpubCachePxc(files);
  for (const auto& name : files) {
    TEST_ASSERT_FALSE_MESSAGE(name.size() >= 6 && name.compare(name.size() - 6, 6, "_q.pxc") == 0,
                              name.c_str());
  }
}

void test_filter_drops_shadowed_pxc() {
  std::vector<std::string> files = sampleListing();
  MyLibraryActivity::filterEpubCachePxc(files);
  for (const auto& name : files) {
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, name.compare("image1.pxc"), "image1.pxc should be filtered");
  }
}

void test_filter_keeps_user_pxc_alongside_bmp() {
  std::vector<std::string> files = sampleListing();
  MyLibraryActivity::filterEpubCachePxc(files);
  bool keptPxc = false, keptBmp = false;
  for (const auto& name : files) {
    if (name == "wallpaper1.pxc") keptPxc = true;
    if (name == "wallpaper1.bmp") keptBmp = true;
  }
  TEST_ASSERT_TRUE(keptPxc);
  TEST_ASSERT_TRUE(keptBmp);
}

void test_filter_keeps_lonely_pxc() {
  std::vector<std::string> files = sampleListing();
  MyLibraryActivity::filterEpubCachePxc(files);
  bool kept = false;
  for (const auto& name : files) {
    if (name == "lonely.pxc") kept = true;
  }
  TEST_ASSERT_TRUE(kept);
}

void test_filter_leaves_non_image_files_alone() {
  std::vector<std::string> files = sampleListing();
  MyLibraryActivity::filterEpubCachePxc(files);
  bool keptEpub = false, keptTxt = false, keptDir = false;
  for (const auto& name : files) {
    if (name == "book.epub") keptEpub = true;
    if (name == "notes.txt") keptTxt = true;
    if (name == "subdir/") keptDir = true;
  }
  TEST_ASSERT_TRUE(keptEpub);
  TEST_ASSERT_TRUE(keptTxt);
  TEST_ASSERT_TRUE(keptDir);
}

}  // namespace

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_filter_drops_q_pxc);
  RUN_TEST(test_filter_drops_shadowed_pxc);
  RUN_TEST(test_filter_keeps_user_pxc_alongside_bmp);
  RUN_TEST(test_filter_keeps_lonely_pxc);
  RUN_TEST(test_filter_leaves_non_image_files_alone);
  return UNITY_END();
}
```

If the test fails to link because `MyLibraryActivity` pulls in display dependencies, extract `filterEpubCachePxc` into a free function in a small dedicated unit (`src/activities/home/LibraryListingFilter.{h,cpp}`) and have `MyLibraryActivity` call it. Update the test to include that header. Refactor only if the simpler arrangement fails to link.

- [ ] **Step 6: Run the tests.**

Run: `pio test -e test_host -f test_library_filter`
Expected: 5 pass.

- [ ] **Step 7: Commit.**

```bash
git add src/activities/home/MyLibraryActivity.h src/activities/home/MyLibraryActivity.cpp test/test_library_filter/
git commit -m "feat(library): hide EPUB-cache .pxc files from browser"
```

---

## Task 11: Web `/files` listing — add `isPxc`, apply cache filter, render badge

**Files:**
- Modify: `src/network/CrossPointWebServer.cpp`
- Modify: `src/network/html/FilesPage.html`

- [ ] **Step 1: Add `isPxc` to the listing struct.**

In `src/network/CrossPointWebServer.cpp`, find the struct that holds `info.isBmp` (around the line with `info.isBmp = hasExtCI(name, nameLen, ".bmp", 4);` near line 777). Add an adjacent field and population line:

```cpp
        info.isBmp = hasExtCI(name, nameLen, ".bmp", 4);
        info.isPxc = hasExtCI(name, nameLen, ".pxc", 4);
```

Also add the `isPxc` field to the struct definition (search up from line 777 for the matching struct; mirror `isBmp` exactly).

- [ ] **Step 2: Emit `isPxc` in the JSON listing payload.**

Where the listing serializer writes `"isBmp"`, add a parallel `"isPxc"` boolean. Match the existing JSON style.

- [ ] **Step 3: Apply the EPUB-cache filter to web listings.**

Right before the JSON serialization loop, run the same logic as `MyLibraryActivity::filterEpubCachePxc` against the listing entries: drop any `*_q.pxc`, drop any `*.pxc` whose stem matches a sibling `.jpg/.jpeg/.png/.gif/.webp` in the same listing.

If the listing struct is local to that function, inline the filter. If you'd rather share with the native browser, factor `filterEpubCachePxc` into a free helper in a new `src/util/ImageListingFilter.{h,cpp}` and call it from both sites — recommended, but only if it's a 5-minute change. If it's not, inline here and leave the duplication for a follow-up.

- [ ] **Step 4: Render the PXC badge in the HTML.**

In `src/network/html/FilesPage.html`:

- Find the `.bmp-file` CSS rules (around lines 245-251) and add parallel `.pxc-file` rules. Use the same colors and hover treatment to keep visual parity.
- Find the `.bmp-badge` CSS rule (line 251) and add a parallel `.pxc-badge` rule.
- In the desktop responsive override block (around line 1205) add `.pxc-badge` next to `.bmp-badge`.
- In the folder-contents override block (around lines 1571 / 1580) add `.pxc-file` next to `.bmp-file`.
- In the row rendering JS (around line 2600), update the row class selection:

```js
const rowClass = file.isEpub
    ? 'epub-file'
    : (file.isBmp ? 'bmp-file' : (file.isPxc ? 'pxc-file' : ''));
```

- And the badge emit (around line 2606):

```js
if (file.isBmp) fileTableContent += '<span class="bmp-badge">BMP</span>';
if (file.isPxc) fileTableContent += '<span class="pxc-badge">PXC</span>';
```

- [ ] **Step 5: Build.**

Run: `pio run -e default`
Expected: build succeeds. The hashed CSS/HTML index regenerates as part of the build.

- [ ] **Step 6: Commit.**

```bash
git add src/network/CrossPointWebServer.cpp src/network/html/FilesPage.html
git commit -m "feat(web): list .pxc files with cache filter and PXC badge"
```

---

## Task 12: Manual on-device verification

**Files:**
- None (verification only)

The renderer, the viewer, the sleep flow, and the web UI all need real-device confirmation. This task is a checklist; nothing is committed.

- [ ] **Step 1: Build production firmware.**

```bash
pio run -e default
```

Expected: build succeeds. Compare Flash% / RAM% to the previous tag and note the delta.

- [ ] **Step 2: Flash to device.**

```bash
pio run -e default -t upload --upload-port /dev/cu.usbmodem101
```

(Adjust the port if `ls /dev/cu.usb*` shows a different one.)

- [ ] **Step 3: Drop test images on the SD card.**

Place these on the SD card (substitute the actual screen dimensions for the device):

- `/sleep/test1.pxc` (screen-sized, valid PXC)
- `/sleep/test1.bmp` (different image, screen-sized BMP)
- `/test2.pxc` at the SD root
- `/Books/coversample.pxc` (an arbitrary screen-sized PXC outside `/sleep`)
- `/Books/some-cached-book/page1.jpg` and `/Books/some-cached-book/page1.pxc` (forces the EPUB-cache filter)
- `/Books/some-cached-book/anything_q.pxc` (forces the `_q.pxc` filter)
- `/Wallpapers/badsize.pxc` (a PXC whose declared dimensions are wrong — e.g. 100×100 — to test the size-mismatch error path)

- [ ] **Step 4: Walk the verification checklist.**

For each, capture the result inline:

- [ ] `/sleep/test1.pxc` and `/sleep/test1.bmp` both visible in library browser.
- [ ] Open `/sleep/test1.pxc` in the viewer — renders fast (single HALF_REFRESH), no garbage.
- [ ] Favorite `/sleep/test1.pxc` — file renames to `test1_F.pxc`, `[F]` prefix appears in browser.
- [ ] Unfavorite — renames back to `test1.pxc`.
- [ ] Rename `test1.pxc` to `renamed` via keyboard — file becomes `renamed.pxc` (extension preserved).
- [ ] Delete `/sleep/test1.pxc` — disappears from listing and from `state.json` favorites.
- [ ] Open `/Books/some-cached-book/`. Verify `page1.pxc` and `anything_q.pxc` are NOT visible. Verify `page1.jpg` IS visible.
- [ ] Open `/Books/coversample.pxc` (no sibling jpg) — visible, opens, behaves as a normal image.
- [ ] Open `/Wallpapers/badsize.pxc` — viewer shows the invalid-image error, back works.
- [ ] Enter sleep with a `.pxc` selected from the playlist; pause sleep (button); resume. Same `.pxc` re-renders.
- [ ] Connect to the web UI. `/files` lists the test PXC files with the `PXC` badge. `_q.pxc` and `page1.pxc` (shadowed) are absent. PXC files are downloadable.
- [ ] Reboot the device. Verify previously-favorited PXC files still show `[F]` (state.json round-trip).

- [ ] **Step 5: If anything fails, re-open the relevant earlier task and fix.**

Don't move on with a known broken step.

---

## Self-review notes

After this plan was drafted I checked it against the spec:

- **Spec coverage:** All sections of the spec map to a task. Section 2's "no state migration" is delivered by Task 3 (rename without touching the JSON key) plus the explicit Task 12 boot-with-existing-state check. Section 3's renamed C++ symbols are covered by Tasks 3 + 6. The PXC renderer extraction is Task 2. The viewer fast path is Task 8. The rename-extension fix is Task 9. The cache filter is Task 10 (native) + Task 11 (web). The web UI is Task 11.
- **Type consistency:** `renderImageView` (defined Task 8), `renderBmpImageView` and `renderPxcImageView` (defined Task 8), `imageViewFullyLoaded` (renamed Task 6), `Mode::IMAGE_VIEW` (renamed Task 6), `enterImageView` (renamed Task 6), `renameSelectedImage` (renamed Task 6, extended Task 9), `FavoriteImage::isImagePath` (renamed Task 4), `SetFavoriteResult::NotImage` (renamed Task 3), `filterEpubCachePxc` (added Task 10), `info.isPxc` (added Task 11). All consistent across tasks.
- **Scope:** This is one feature plan, not multiple. No decomposition needed.
