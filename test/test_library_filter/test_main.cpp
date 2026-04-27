// Host-side tests for the EPUB-cache PXC filter on library listings.
// Run via: pio test -e test_host -f test_library_filter

#include <unity.h>

#include <string>
#include <vector>

#include "activities/home/LibraryListingFilter.h"

void setUp() {}
void tearDown() {}

namespace {

std::vector<std::string> sampleListing() {
  return {
      "wallpaper1.pxc",
      "wallpaper1.bmp",
      "lonely.pxc",
      "image1.jpg",
      "image1.pxc",
      "anything_q.pxc",
      "book.epub",
      "notes.txt",
      "subdir/",
  };
}

void test_filter_drops_q_pxc() {
  std::vector<std::string> files = sampleListing();
  LibraryListingFilter::filterEpubCachePxc(files);
  for (const auto& name : files) {
    TEST_ASSERT_FALSE_MESSAGE(name.size() >= 6 && name.compare(name.size() - 6, 6, "_q.pxc") == 0,
                              name.c_str());
  }
}

void test_filter_drops_shadowed_pxc() {
  std::vector<std::string> files = sampleListing();
  LibraryListingFilter::filterEpubCachePxc(files);
  for (const auto& name : files) {
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, name.compare("image1.pxc"), "image1.pxc should be filtered");
  }
}

void test_filter_keeps_user_pxc_alongside_bmp() {
  std::vector<std::string> files = sampleListing();
  LibraryListingFilter::filterEpubCachePxc(files);
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
  LibraryListingFilter::filterEpubCachePxc(files);
  bool kept = false;
  for (const auto& name : files) {
    if (name == "lonely.pxc") kept = true;
  }
  TEST_ASSERT_TRUE(kept);
}

void test_filter_leaves_non_image_files_alone() {
  std::vector<std::string> files = sampleListing();
  LibraryListingFilter::filterEpubCachePxc(files);
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
