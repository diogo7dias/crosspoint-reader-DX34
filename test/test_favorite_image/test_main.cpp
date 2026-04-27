// Host-side tests for FavoriteImage suffix helpers.
// Run via: pio test -e test_host -f test_favorite_image

#include <unity.h>

#include <string>

#include "util/FavoriteImageNames.h"

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
