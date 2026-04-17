#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace LibrarySearchSupport {

std::string searchLabelForEntry(const std::string& entry);

std::vector<size_t> rankMatches(const std::vector<std::string>& entries, const std::string& query);

}  // namespace LibrarySearchSupport
