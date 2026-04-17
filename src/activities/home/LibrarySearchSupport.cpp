#include "LibrarySearchSupport.h"

#include <esp_task_wdt.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <tuple>
#include <vector>

namespace LibrarySearchSupport {
namespace {

char toLowerAscii(const char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string toLowerAsciiCopy(const std::string& text) {
  std::string lowered = text;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const char c) { return toLowerAscii(c); });
  return lowered;
}

bool startsWithIgnoreCase(const std::string& text, const std::string& prefix) {
  if (prefix.size() > text.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (toLowerAscii(text[i]) != toLowerAscii(prefix[i])) {
      return false;
    }
  }
  return true;
}

bool isWordBoundary(const std::string& text, const size_t index) {
  if (index == 0) {
    return true;
  }
  return !std::isalnum(static_cast<unsigned char>(text[index - 1]));
}

int findWordBoundaryPrefix(const std::string& text, const std::string& query) {
  if (query.empty() || query.size() > text.size()) {
    return -1;
  }

  for (size_t pos = 0; pos + query.size() <= text.size(); ++pos) {
    if (!isWordBoundary(text, pos)) {
      continue;
    }
    if (startsWithIgnoreCase(text.substr(pos), query)) {
      return static_cast<int>(pos);
    }
  }

  return -1;
}

struct SubsequenceScore {
  bool matched = false;
  int start = std::numeric_limits<int>::max();
  int span = std::numeric_limits<int>::max();
  int gaps = std::numeric_limits<int>::max();
};

SubsequenceScore computeSubsequenceScore(const std::string& loweredText, const std::string& loweredQuery) {
  if (loweredQuery.empty()) {
    return {.matched = true, .start = 0, .span = 0, .gaps = 0};
  }

  SubsequenceScore best;
  for (size_t startPos = 0; startPos < loweredText.size(); ++startPos) {
    if (loweredText[startPos] != loweredQuery[0]) {
      continue;
    }

    size_t queryPos = 1;
    size_t textPos = startPos + 1;
    size_t lastMatch = startPos;
    while (queryPos < loweredQuery.size() && textPos < loweredText.size()) {
      if (loweredText[textPos] == loweredQuery[queryPos]) {
        lastMatch = textPos;
        ++queryPos;
      }
      ++textPos;
    }

    if (queryPos != loweredQuery.size()) {
      continue;
    }

    const int start = static_cast<int>(startPos);
    const int span = static_cast<int>(lastMatch - startPos);
    const int gaps = span - static_cast<int>(loweredQuery.size()) + 1;

    if (!best.matched || std::tie(start, span, gaps) < std::tie(best.start, best.span, best.gaps)) {
      best = {.matched = true, .start = start, .span = span, .gaps = gaps};
    }
  }

  return best;
}

struct MatchCandidate {
  size_t fileIndex = 0;
  int category = 0;
  int start = 0;
  int span = 0;
  int gaps = 0;
};

}  // namespace

std::string searchLabelForEntry(const std::string& entry) {
  if (!entry.empty() && entry.back() == '/') {
    return entry.substr(0, entry.size() - 1);
  }
  return entry;
}

std::vector<size_t> rankMatches(const std::vector<std::string>& entries, const std::string& query) {
  if (query.empty()) {
    return {};
  }

  const std::string loweredQuery = toLowerAsciiCopy(query);
  std::vector<MatchCandidate> matches;
  matches.reserve(entries.size());

  for (size_t fileIndex = 0; fileIndex < entries.size(); ++fileIndex) {
    if (fileIndex % 100 == 0) {
      esp_task_wdt_reset();
    }
    const std::string label = searchLabelForEntry(entries[fileIndex]);
    const std::string loweredLabel = toLowerAsciiCopy(label);

    if (startsWithIgnoreCase(label, query)) {
      matches.push_back({.fileIndex = fileIndex, .category = 0, .start = 0, .span = 0, .gaps = 0});
      continue;
    }

    const int boundaryPrefix = findWordBoundaryPrefix(label, query);
    if (boundaryPrefix >= 0) {
      matches.push_back({.fileIndex = fileIndex,
                         .category = 1,
                         .start = boundaryPrefix,
                         .span = static_cast<int>(query.size()) - 1,
                         .gaps = 0});
      continue;
    }

    const auto subsequence = computeSubsequenceScore(loweredLabel, loweredQuery);
    if (!subsequence.matched) {
      continue;
    }

    matches.push_back({.fileIndex = fileIndex,
                       .category = 2,
                       .start = subsequence.start,
                       .span = subsequence.span,
                       .gaps = subsequence.gaps});
  }

  std::stable_sort(matches.begin(), matches.end(), [](const MatchCandidate& lhs, const MatchCandidate& rhs) {
    return std::tie(lhs.category, lhs.start, lhs.span, lhs.gaps) <
           std::tie(rhs.category, rhs.start, rhs.span, rhs.gaps);
  });

  std::vector<size_t> rankedFileIndexes;
  rankedFileIndexes.reserve(matches.size());
  std::transform(matches.begin(), matches.end(), std::back_inserter(rankedFileIndexes),
                 [](const MatchCandidate& match) { return match.fileIndex; });
  return rankedFileIndexes;
}

}  // namespace LibrarySearchSupport
