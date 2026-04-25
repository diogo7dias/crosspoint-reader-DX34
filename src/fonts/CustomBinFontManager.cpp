#include "CustomBinFontManager.h"

#include <EpdBinFontLoader.h>
#include <EpdBinFormat.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "CrossPointState.h"
#include "CustomBinFontIds.h"

namespace crosspoint {
namespace fonts {

namespace {
constexpr const char* kModule = "CBFONT";
constexpr const char* kCustomFontDir = "/custom-font";
constexpr size_t kLegacyScanCap = 2000;
constexpr size_t kFamilyScanCap = 64;
constexpr size_t kVariantScanCap = 64;  // 16 sizes × 4 variants

bool endsWithCI(const char* name, const char* suffix) {
  if (!name || !suffix) return false;
  const size_t n = std::strlen(name);
  const size_t s = std::strlen(suffix);
  return n >= s && strcasecmp(name + n - s, suffix) == 0;
}

bool isLegacyFontName(const char* name) { return endsWithCI(name, ".bdf") || endsWithCI(name, ".idx"); }

size_t walkAndDeleteLegacy(const char* dirPath, int depth) {
  if (depth > 1) return 0;
  auto dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  size_t removed = 0, iter = 0;
  char name[256];
  char fullPath[320];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    file.close();
    if (isDir) {
      if (name[0] == '\0' || name[0] == '.') continue;
      snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, name);
      removed += walkAndDeleteLegacy(fullPath, depth + 1);
    } else if (isLegacyFontName(name)) {
      snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, name);
      if (Storage.remove(fullPath)) ++removed;
    }
    if (++iter % 32 == 0) {
      esp_task_wdt_reset();
      yield();
    }
    if (iter > kLegacyScanCap) break;
  }
  dir.close();
  return removed;
}

struct VariantTag {
  const char* prefix;
  uint8_t variant;
};
constexpr VariantTag kTags[] = {
    {"regular_", binfont::kVariantRegular},
    {"bold_", binfont::kVariantBold},
    {"italic_", binfont::kVariantItalic},
    {"bolditalic_", binfont::kVariantBoldItalic},
};

const char* variantPrefix(uint8_t v) { return v <= binfont::kVariantBoldItalic ? kTags[v].prefix : "regular_"; }

// Configured legal point-size range. Files outside this range get
// auto-deleted by scan() so a firmware revision can't leave stale
// files that the current build's picker won't offer.
constexpr uint16_t kMinSizePt = 25;
constexpr uint16_t kMaxSizePt = 40;

bool parseVariantFile(const char* name, uint8_t& outVariant, uint16_t& outSize) {
  for (const auto& t : kTags) {
    const size_t pl = std::strlen(t.prefix);
    if (strncasecmp(name, t.prefix, pl) != 0) continue;
    const char* rest = name + pl;
    char* end = nullptr;
    const long sz = std::strtol(rest, &end, 10);
    if (end == rest) return false;
    if (strcasecmp(end, ".bin") != 0) return false;
    if (sz < kMinSizePt || sz > kMaxSizePt) return false;
    outVariant = t.variant;
    outSize = static_cast<uint16_t>(sz);
    return true;
  }
  return false;
}

// Returns true when `name` looks like a variant file but its parsed
// size sits outside [kMinSizePt, kMaxSizePt]. Used by scan() to
// auto-delete leftover files from a wider-range firmware.
bool isStaleSizeFile(const char* name) {
  for (const auto& t : kTags) {
    const size_t pl = std::strlen(t.prefix);
    if (strncasecmp(name, t.prefix, pl) != 0) continue;
    const char* rest = name + pl;
    char* end = nullptr;
    const long sz = std::strtol(rest, &end, 10);
    if (end == rest) return false;
    if (strcasecmp(end, ".bin") != 0) return false;
    return sz < kMinSizePt || sz > kMaxSizePt;
  }
  return false;
}

void insertVariant(std::vector<CustomBinFontSize>& sizes, uint8_t variant, uint16_t sizePt) {
  CustomBinFontSize* target = nullptr;
  for (auto& s : sizes) {
    if (s.sizePt == sizePt) {
      target = &s;
      break;
    }
  }
  if (!target) {
    sizes.push_back({sizePt, false, false, false, false});
    target = &sizes.back();
  }
  switch (variant) {
    case binfont::kVariantRegular:
      target->hasRegular = true;
      break;
    case binfont::kVariantBold:
      target->hasBold = true;
      break;
    case binfont::kVariantItalic:
      target->hasItalic = true;
      break;
    case binfont::kVariantBoldItalic:
      target->hasBoldItalic = true;
      break;
  }
}

std::string familyDir(const std::string& name) { return std::string(kCustomFontDir) + "/" + name; }

std::string variantPath(const std::string& name, uint8_t variant, uint16_t sizePt) {
  char tail[32];
  snprintf(tail, sizeof(tail), "/%s%u.bin", variantPrefix(variant), static_cast<unsigned>(sizePt));
  return familyDir(name) + tail;
}

// Remove the family dir if it holds nothing but dot entries. Caller
// owns validity of `name`.
void rmdirIfEmpty(const std::string& name) {
  const std::string dirPath = familyDir(name);
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  bool empty = true;
  char entryName[256];
  for (auto e = dir.openNextFile(); e; e = dir.openNextFile()) {
    e.getName(entryName, sizeof(entryName));
    const bool isDot = entryName[0] == '.' && (entryName[1] == '\0' || (entryName[1] == '.' && entryName[2] == '\0'));
    e.close();
    if (!isDot) {
      empty = false;
      break;
    }
  }
  dir.close();
  if (empty) Storage.rmdir(dirPath.c_str());
}

}  // namespace

bool isValidFamilyName(const std::string& name) {
  if (name.empty() || name.size() > 32) return false;
  if (!std::isalnum(static_cast<unsigned char>(name[0]))) return false;
  for (size_t i = 1; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (!std::isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}

CustomBinFontManager& CustomBinFontManager::instance() {
  static CustomBinFontManager inst;
  return inst;
}

void CustomBinFontManager::scan() {
  families_.clear();
  auto root = Storage.open(kCustomFontDir);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  char name[256];
  size_t familyCount = 0;
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    entry.close();
    if (!isDir || name[0] == '.' || name[0] == '\0') continue;
    if (!isValidFamilyName(name)) continue;
    if (++familyCount > kFamilyScanCap) break;

    CustomBinFontFamily fam;
    fam.name = name;
    const std::string subDirPath = familyDir(fam.name);
    auto subDir = Storage.open(subDirPath.c_str());
    if (!subDir || !subDir.isDirectory()) {
      if (subDir) subDir.close();
      continue;
    }

    char varName[256];
    size_t seen = 0;
    for (auto vf = subDir.openNextFile(); vf; vf = subDir.openNextFile()) {
      vf.getName(varName, sizeof(varName));
      const bool vfIsDir = vf.isDirectory();
      vf.close();
      if (vfIsDir) continue;
      // Stale files from a previous, wider-range firmware are deleted
      // here so the inventory only ever lists sizes the current build
      // can actually render.
      if (isStaleSizeFile(varName)) {
        const std::string p = subDirPath + "/" + varName;
        if (Storage.remove(p.c_str())) {
          LOG_INF(kModule, "scan: deleted out-of-range %s", p.c_str());
        }
        continue;
      }
      uint8_t variant;
      uint16_t sizePt;
      if (!parseVariantFile(varName, variant, sizePt)) continue;
      insertVariant(fam.sizes, variant, sizePt);
      if (++seen > kVariantScanCap) break;
      if (seen % 16 == 0) {
        esp_task_wdt_reset();
        yield();
      }
    }
    subDir.close();

    if (!fam.sizes.empty()) {
      std::sort(fam.sizes.begin(), fam.sizes.end(),
                [](const CustomBinFontSize& a, const CustomBinFontSize& b) { return a.sizePt < b.sizePt; });
      families_.push_back(std::move(fam));
    }
  }
  root.close();
  LOG_INF(kModule, "scan: %u families", static_cast<unsigned>(families_.size()));
}

std::vector<std::string> CustomBinFontManager::familyNames() const {
  std::vector<std::string> out;
  out.reserve(families_.size());
  for (const auto& f : families_) out.push_back(f.name);
  return out;
}

std::vector<uint8_t> CustomBinFontManager::installedSizesFor(const std::string& name) const {
  for (const auto& f : families_) {
    if (f.name != name) continue;
    std::vector<uint8_t> sizes;
    sizes.reserve(f.sizes.size());
    for (const auto& s : f.sizes) {
      if (s.hasRegular) sizes.push_back(static_cast<uint8_t>(s.sizePt));
    }
    return sizes;
  }
  return {};
}

void CustomBinFontManager::clearActive() {
  if (!active_) return;
  if (renderer_) {
    renderer_->removeFont(active_->fontId);
    active_.reset();
  } else {
    // Freeing active_ would leave dangling pointers in the renderer's
    // fontMap. Leak until a later activate()/deactivate() cycle can
    // swap cleanly.
    LOG_ERR(kModule, "clearActive without renderer_; leaking active family to avoid UAF");
  }
}

bool CustomBinFontManager::activate(const std::string& name, uint16_t sizePt) {
  if (!renderer_) {
    LOG_ERR(kModule, "activate before setRenderer()");
    return false;
  }
  if (!isValidFamilyName(name)) return false;
  if (active_ && active_->name == name && active_->sizePt == sizePt) return true;

  auto fresh = std::make_unique<ActiveFamily>();
  fresh->name = name;
  fresh->sizePt = sizePt;
  fresh->fontId = idForFamily(name, sizePt);

  const std::string regPath = variantPath(name, binfont::kVariantRegular, sizePt);
  if (!Storage.exists(regPath.c_str())) {
    LOG_ERR(kModule, "activate: regular missing at %s", regPath.c_str());
    return false;
  }

  auto openSlot = [&](uint8_t variant) -> bool {
    const std::string p = variantPath(name, variant, sizePt);
    if (!Storage.exists(p.c_str())) return true;
    auto loader = std::make_unique<binfont::EpdBinFontLoader>();
    if (!loader->openFromFile(p)) {
      LOG_ERR(kModule, "open %s failed: %s", p.c_str(), loader->lastError().c_str());
      return variant == binfont::kVariantRegular ? false : true;
    }
    fresh->fonts[variant] = std::make_unique<EpdFont>(loader->data());
    fresh->loaders[variant] = std::move(loader);
    return true;
  };

  if (!openSlot(binfont::kVariantRegular)) return false;
  openSlot(binfont::kVariantBold);
  openSlot(binfont::kVariantItalic);
  openSlot(binfont::kVariantBoldItalic);

  if (!fresh->fonts[binfont::kVariantRegular]) return false;

  // SD-streamed loaders only hold a few KB of tables in heap each,
  // so the four-variant load is back to being the default. Bold +
  // italic + boldItalic uploaded by the browser display verbatim;
  // missing slots fall back via EpdFontFamily::getFont.
  EpdFontFamily family(fresh->fonts[binfont::kVariantRegular].get(), fresh->fonts[binfont::kVariantBold].get(),
                       fresh->fonts[binfont::kVariantItalic].get(), fresh->fonts[binfont::kVariantBoldItalic].get());

  clearActive();
  renderer_->insertFont(fresh->fontId, family);
  active_ = std::move(fresh);
  LOG_INF(kModule, "activated %s size=%u id=%d", name.c_str(), static_cast<unsigned>(sizePt), active_->fontId);
  return true;
}

void CustomBinFontManager::deactivate() { clearActive(); }

// static
bool CustomBinFontManager::validateInstalledRegular(const std::string& name, uint16_t sizePt) {
  if (!isValidFamilyName(name)) return false;
  const std::string p = variantPath(name, binfont::kVariantRegular, sizePt);
  if (!Storage.exists(p.c_str())) return false;
  std::string err;
  const bool ok = binfont::EpdBinFontLoader::validateFile(p, &err);
  if (!ok) {
    LOG_DIAG(kModule, "validateInstalledRegular fail name=%s size=%u err=%s", name.c_str(),
             static_cast<unsigned>(sizePt), err.c_str());
  }
  return ok;
}

size_t CustomBinFontManager::deleteFamilySize(const std::string& name, uint16_t sizePt) {
  if (!isValidFamilyName(name)) return 0;
  if (active_ && active_->name == name && active_->sizePt == sizePt) clearActive();
  size_t removed = 0;
  for (uint8_t v = 0; v <= binfont::kVariantBoldItalic; ++v) {
    const std::string p = variantPath(name, v, sizePt);
    if (Storage.exists(p.c_str()) && Storage.remove(p.c_str())) ++removed;
  }
  rmdirIfEmpty(name);
  scan();
  return removed;
}

size_t CustomBinFontManager::deleteFamily(const std::string& name) {
  if (!isValidFamilyName(name)) return 0;
  if (active_ && active_->name == name) clearActive();
  const std::string dirPath = familyDir(name);
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  size_t removed = 0;
  char entryName[256];
  for (auto e = dir.openNextFile(); e; e = dir.openNextFile()) {
    e.getName(entryName, sizeof(entryName));
    const bool isDir = e.isDirectory();
    e.close();
    if (isDir) continue;
    if (Storage.remove((dirPath + "/" + entryName).c_str())) ++removed;
  }
  dir.close();
  Storage.rmdir(dirPath.c_str());
  scan();
  return removed;
}

size_t cleanupLegacyBdfFiles() {
  auto& state = CrossPointState::getInstance();
  if (state.customFontLegacyCleanupDone) return 0;
  const size_t removed = walkAndDeleteLegacy(kCustomFontDir, 0);
  state.customFontLegacyCleanupDone = 1;
  state.saveToFile();
  if (removed > 0) LOG_INF(kModule, "legacy cleanup removed %u files", static_cast<unsigned>(removed));
  return removed;
}

}  // namespace fonts
}  // namespace crosspoint
