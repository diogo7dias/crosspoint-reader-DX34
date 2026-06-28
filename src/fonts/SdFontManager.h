#pragma once

// SD-backed reader-font manager — the generalization of the single-font pilot
// (SdFontPilot) to every offloadable reader-font family.
//
// Built-in reader fonts are large (~2 MB of flash across Bookerly/Georgia/Lato/
// Helvetica/Verdana). With -DCROSSPOINT_SD_FONTS_SLIM the per-glyph bitmap blob
// of each variant is compiled out of flash (see scripts/offload_bitmaps.py) and
// streamed instead from a CPBN `.bin` pack on the SD card. The glyph / interval /
// group / kerning TABLES always stay in flash; only the DEFLATE bitmap moves.
//
// Handle scaling: there is exactly one active reader font at a time (the id from
// CrossPointSettings::getReaderFontId(), a family at one size). Only that font's
// weight variants (regular/bold/italic/[bolditalic], <= 4) ever render, so only
// those few `.bin` files are held open. ensureActive() swaps the open set when
// the selection changes and is a no-op when it does not — so a page-turn within
// the same font opens no files and stays snappy.
//
// This class is deliberately HAL-free: all filesystem work goes through an
// injected SdFontIo (the device wires a HalFile/Storage adapter; host tests wire
// a std::map). That keeps the whole registry / activate / restore / fallback
// state machine unit-testable on the host.

#include <cstdint>
#include <cstdio>

#include "EpdBinExport.h"       // exportFontBlob, glyphCountFromIntervals
#include "EpdBinFontLoader.h"   // EpdBinFontLoader, BlobSource
#include "EpdBinFormat.h"       // FontBlobExpectation, BlobReject
#include "EpdFont.h"
#include "EpdFontData.h"

namespace crosspoint {
namespace fonts {

// Filesystem seam. The manager never touches the HAL directly; it asks an SdFontIo
// to check / write / open CPBN packs. `openSource` returns a BlobSource the IO
// owns; the manager hands it back via `releaseSource` when the font deactivates.
struct SdFontIo {
  virtual ~SdFontIo() = default;
  virtual bool exists(const char* path) = 0;
  // Serialize an in-flash font's bitmap blob to a CPBN pack at `path`. Only ever
  // called for fonts whose flash bitmap is present (i.e. a non-slim build).
  virtual bool exportBlob(const char* path, const EpdFontData& flashData, uint16_t sizePt) = 0;
  // Open `path` for streaming reads. Returns nullptr if absent/unreadable.
  virtual binfont::BlobSource* openSource(const char* path) = 0;
  virtual void releaseSource(binfont::BlobSource* src) = 0;
};

class SdFontManager {
 public:
  static constexpr int kMaxWeights = 4;  // regular / bold / italic / bolditalic
  static constexpr int kMaxFonts = 32;   // 5 families x 5 sizes = 25; rounded up
  static constexpr int kPathMax = 64;
  static constexpr int kNoFont = -1;

  void setIo(SdFontIo* io) { io_ = io; }

  // Register one reader font id (a family at one size). Pass the live, non-const
  // EpdFont globals the renderer dereferences; nullptr weights are skipped. The
  // current `font->data` is captured as the flash source for export/restore.
  // `slimFallback` is substituted in a slim build if the SD pack can't be loaded
  // (so the renderer never dereferences a null bitmap); pass nullptr in fat builds.
  void registerFont(int fontId, uint16_t sizePt, const char* stem, EpdFont* regular, EpdFont* bold, EpdFont* italic,
                    EpdFont* boldItalic, const EpdFontData* slimFallback) {
    if (fontCount_ >= kMaxFonts) return;
    Font& f = fonts_[fontCount_++];
    f.fontId = fontId;
    f.sizePt = sizePt;
    f.stem = stem;
    f.fallback = slimFallback;
    f.weightCount = 0;
    addWeight(f, regular, "regular");
    addWeight(f, bold, "bold");
    addWeight(f, italic, "italic");
    addWeight(f, boldItalic, "bolditalic");
  }

  // Write any missing CPBN pack for every registered variant (the migration step
  // run once on the fat "export" build). Variants whose flash bitmap was dropped
  // (slim build) are skipped — there is nothing to serialize from.
  void exportAllMissing() {
    if (io_ == nullptr) return;
    char path[kPathMax];
    for (int i = 0; i < fontCount_; ++i) {
      Font& f = fonts_[i];
      for (int w = 0; w < f.weightCount; ++w) {
        const Weight& wt = f.weights[w];
        if (wt.flashData->bitmap == nullptr) continue;  // slim: can't export
        buildPath(path, f.stem, f.sizePt, wt.suffix);
        if (io_->exists(path)) continue;
        io_->exportBlob(path, *wt.flashData, f.sizePt);
      }
    }
  }

  // Make `fontId` the live SD-backed set. Closes the previously active set first
  // (restoring those fonts to flash), then opens this font's packs. A no-op when
  // `fontId` is already active. An unregistered id (e.g. a ChareInk emergency
  // downgrade) simply closes the previous set and backs nothing new.
  void ensureActive(int fontId) {
    if (fontId == activeFontId_) return;
    deactivateAll();
    activeFontId_ = fontId;
    Font* f = find(fontId);
    if (f == nullptr || io_ == nullptr) return;

    char path[kPathMax];
    for (int w = 0; w < f->weightCount && w < kMaxWeights; ++w) {
      Weight& wt = f->weights[w];
      Slot& s = slots_[w];
      s.fallback = f->fallback;

      buildPath(path, f->stem, f->sizePt, wt.suffix);
      // Fat build self-heal: export a pack that is somehow missing on demand.
      if (wt.flashData->bitmap != nullptr && !io_->exists(path)) {
        io_->exportBlob(path, *wt.flashData, f->sizePt);
      }
      if (!activateWeight(s, wt, path)) {
        // SD load failed: fat build keeps its flash bitmap; slim build (no flash
        // bitmap) substitutes the guaranteed fallback font.
        wt.font->data = (wt.flashData->bitmap != nullptr) ? wt.flashData : f->fallback;
      }
    }
  }

  int activeFontId() const { return activeFontId_; }

 private:
  struct Weight {
    EpdFont* font;                // live object the renderer dereferences
    const EpdFontData* flashData;  // flash source (bitmap present in fat builds)
    const char* suffix;            // "regular" / "bold" / ...
  };
  struct Font {
    int fontId;
    uint16_t sizePt;
    const char* stem;
    const EpdFontData* fallback;
    Weight weights[kMaxWeights];
    int weightCount;
  };
  // One open SD pack: its loader (holds the BlobSource) and the SD-backed
  // EpdFontData the bound font points at while active.
  struct Slot {
    binfont::EpdBinFontLoader loader;
    binfont::BlobSource* src = nullptr;
    EpdFont* boundFont = nullptr;
    const EpdFontData* flashData = nullptr;
    const EpdFontData* fallback = nullptr;
    EpdFontData sdData{};
  };

  void addWeight(Font& f, EpdFont* font, const char* suffix) {
    if (font == nullptr || f.weightCount >= kMaxWeights) return;
    Weight& w = f.weights[f.weightCount++];
    w.font = font;
    w.flashData = font->data;
    w.suffix = suffix;
  }

  static void buildPath(char* out, const char* stem, uint16_t sizePt, const char* suffix) {
    std::snprintf(out, kPathMax, "/fonts/%s_%u_%s.bin", stem, static_cast<unsigned>(sizePt), suffix);
  }

  Font* find(int fontId) {
    for (int i = 0; i < fontCount_; ++i) {
      if (fonts_[i].fontId == fontId) return &fonts_[i];
    }
    return nullptr;
  }

  // Open + validate one pack and repoint the font at an SD-backed copy. Returns
  // false (leaving the font untouched) on any open/validation failure.
  bool activateWeight(Slot& s, Weight& wt, const char* path) {
    s.src = io_->openSource(path);
    if (s.src == nullptr) return false;
    const binfont::FontBlobExpectation expect{binfont::glyphCountFromIntervals(*wt.flashData),
                                              wt.flashData->intervalCount, wt.flashData->groupCount};
    if (s.loader.open(s.src, expect) != binfont::kOk) {
      io_->releaseSource(s.src);
      s.src = nullptr;
      return false;
    }
    s.sdData = *wt.flashData;  // copy every table pointer + metric...
    s.sdData.bitmap = nullptr;  // ...but stream the bitmap from SD.
    s.sdData.bitmapCtx = s.loader.bitmapCtx();
    s.sdData.readBitmapBytes = &binfont::EpdBinFontLoader::readBitmapTrampoline;
    s.boundFont = wt.font;
    s.flashData = wt.flashData;
    wt.font->data = &s.sdData;
    return true;
  }

  void deactivateAll() {
    for (int w = 0; w < kMaxWeights; ++w) {
      Slot& s = slots_[w];
      if (s.src != nullptr) {
        if (io_ != nullptr) io_->releaseSource(s.src);
        s.src = nullptr;
      }
      if (s.boundFont != nullptr && s.flashData != nullptr) {
        // Restore to flash; in a slim build the flash bitmap is null, so fall back.
        s.boundFont->data = (s.flashData->bitmap != nullptr) ? s.flashData : s.fallback;
      }
      s.boundFont = nullptr;
      s.flashData = nullptr;
      s.fallback = nullptr;
    }
  }

  SdFontIo* io_ = nullptr;
  Font fonts_[kMaxFonts];
  int fontCount_ = 0;
  Slot slots_[kMaxWeights];
  int activeFontId_ = kNoFont;
};

// Process-wide instance (ODR-safe across translation units). Only instantiated in
// builds that actually reference it (the SD-font path is flag-gated), so the
// default in-flash build pays nothing.
inline SdFontManager& sdFonts() {
  static SdFontManager instance;
  return instance;
}

}  // namespace fonts
}  // namespace crosspoint
