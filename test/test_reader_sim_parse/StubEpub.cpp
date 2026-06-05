// Define the few REAL symbols the parse slice references without compiling the
// heavy Epub.cpp: Epub::readItemContentsToStream (stubbed to "no image") and
// BookFingerprint::cacheDirName (used by Epub's inline ctor).
#include <BookFingerprint.h>
#include <Epub.h>

bool Epub::readItemContentsToStream(const std::string&, Print&, size_t) const { return false; }

namespace BookFingerprint {
std::string cacheDirName(const char*, const std::string&, const std::string&) { return std::string("/tmp/sim_cache"); }
}  // namespace BookFingerprint
