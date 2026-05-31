#pragma once
#include <string>
namespace BookFingerprint {
std::string cacheDirName(const char* prefix, const std::string& filepath, const std::string& cacheBase);
}
