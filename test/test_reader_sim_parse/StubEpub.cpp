// Define the few REAL symbols the parse slice references without compiling the
// heavy Epub.cpp: the Epub ctor/dtor (now defined out of line because the
// unique_ptr<ZipFile> sessionZip member needs a complete ZipFile type),
// Epub::readItemContentsToStream (stubbed to "no image"), and
// BookFingerprint::cacheDirName (used by the ctor).
#include <BookFingerprint.h>
#include <Epub.h>
#include <ZipFile.h>  // complete type for the unique_ptr<ZipFile> member in ~Epub

Epub::Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
  cachePath = BookFingerprint::cacheDirName("epub", this->filepath, cacheDir);
}

Epub::~Epub() = default;

bool Epub::readItemContentsToStream(const std::string&, Print&, size_t) const { return false; }

namespace BookFingerprint {
std::string cacheDirName(const char*, const std::string&, const std::string&) { return std::string("/tmp/sim_cache"); }
}  // namespace BookFingerprint
