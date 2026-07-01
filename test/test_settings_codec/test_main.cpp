/**
 * Host-side tests for the pure CrossPointSettings JSON codec
 * (src/persist/SettingsCodec.cpp) + the Hal-free logic helpers
 * (src/CrossPointSettingsLogic.cpp). No ESP32/SdFat/Hal — runs on the
 * developer machine via:
 *   pio test -e test_host
 *
 * This suite GUARDS THE settings.json WIRE FORMAT. The on-disk JSON is a hard
 * contract (existing cards must keep parsing), so:
 *   - round-trip identity proves encode/decode are inverses for the live fields;
 *   - the drift-parity test pins the v6.0.0 granular status-bar regression
 *     (those keys were written to themes but not to settings.json);
 *   - the migration tests pin each legacy-upgrade path (resave-on-load);
 *   - the GOLDEN snapshot locks the exact serialized bytes + key order so any
 *     accidental reorder / add / drop of a key trips the test.
 * Every test drives ONLY encodeSettings()/decodeSettings() through the injected
 * SettingsEnv port, so it is fully decoupled from the device wiring.
 */
#include <ArduinoJson.h>
#include <unity.h>

#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "persist/SettingsCodec.h"
#include "persist/SettingsSchema.h"

using crosspoint::persist::decodeSettings;
using crosspoint::persist::encodeSettings;
using crosspoint::persist::SettingsEnv;

// Deterministic, Hal-free environment for the codec: fixed language count,
// identity obfuscation (so password fields stay readable + stable), a
// deobfuscate that always reports success, and a no-op factory-LUT apply.
static SettingsEnv testEnv() {
  return {[] { return (uint8_t)5; }, [](const std::string& p) { return p; },
          [](const std::string& b, bool* ok) {
            if (ok) *ok = true;
            return b;
          },
          [](bool) {}};
}

static std::string encodeToString(const CrossPointSettings& s) {
  JsonDocument doc;
  encodeSettings(s, doc, testEnv());
  std::string out;
  serializeJson(doc, out);
  return out;
}

void setUp() {}
void tearDown() {}

// 1. Round-trip identity: encode default -> serialize -> decode -> fields equal.
void test_round_trip_identity() {
  CrossPointSettings a;  // defaults
  const std::string json = encodeToString(a);

  CrossPointSettings b;
  bool needsResave = true;
  TEST_ASSERT_TRUE(decodeSettings(b, json.c_str(), testEnv(), &needsResave));

  TEST_ASSERT_EQUAL_UINT8(a.fontFamily, b.fontFamily);
  TEST_ASSERT_EQUAL_UINT8(a.fontSize, b.fontSize);
  TEST_ASSERT_EQUAL_UINT8(a.lineSpacingPercent, b.lineSpacingPercent);
  TEST_ASSERT_EQUAL_UINT8(a.wordSpacingPercent, b.wordSpacingPercent);
  TEST_ASSERT_EQUAL_UINT8(a.textRenderMode, b.textRenderMode);
  TEST_ASSERT_EQUAL_UINT8(a.statusBarShowBattery, b.statusBarShowBattery);
  TEST_ASSERT_EQUAL_UINT8(a.orientation, b.orientation);
  TEST_ASSERT_EQUAL_UINT8(a.sleepTimeout, b.sleepTimeout);
  TEST_ASSERT_EQUAL_UINT8(a.imageDither, b.imageDither);
}

// 2. Drift-parity regression (v6.0.0): the granular status-bar toggles must
// survive encode->decode. They were saved to reading_themes.json but NOT to
// settings.json, so toggling them did not survive a reboot. The codec must
// round-trip all six.
void test_drift_parity_granular_status_bar() {
  CrossPointSettings a;
  a.statusBarShowFreeHeap = 1;
  a.statusBarShowPagesLeft = 1;
  a.statusBarShowChapterNumber = 1;
  a.statusBarShowQuoteCount = 1;
  a.statusBarTitleContent = 1;
  const std::string json = encodeToString(a);

  CrossPointSettings b;
  bool needsResave = false;
  TEST_ASSERT_TRUE(decodeSettings(b, json.c_str(), testEnv(), &needsResave));

  TEST_ASSERT_EQUAL_UINT8(1, b.statusBarShowFreeHeap);
  TEST_ASSERT_EQUAL_UINT8(1, b.statusBarShowPagesLeft);
  TEST_ASSERT_EQUAL_UINT8(1, b.statusBarShowChapterNumber);
  TEST_ASSERT_EQUAL_UINT8(1, b.statusBarShowQuoteCount);
  TEST_ASSERT_EQUAL_UINT8(1, b.statusBarTitleContent);
}

// 3. Migration: word-spacing midpoint remap. A pre-midpoint file has
// wordSpacingPercent without the wordSpacingMidpoints flag -> remap + resave.
void test_migration_word_spacing_midpoint() {
  CrossPointSettings s;
  bool needsResave = false;
  TEST_ASSERT_TRUE(decodeSettings(s, "{\"wordSpacingPercent\":2}", testEnv(), &needsResave));
  TEST_ASSERT_TRUE(needsResave);
  TEST_ASSERT_EQUAL_UINT8(CrossPointSettings::migrateWordSpacingToMidpoints(2), s.wordSpacingPercent);
}

// 4. Migration: legacy status bar. A file with only the old `statusBar` enum
// (no granular keys) -> migrateLegacyStatusBarMode path + resave.
void test_migration_legacy_status_bar() {
  CrossPointSettings s;
  bool needsResave = false;
  TEST_ASSERT_TRUE(decodeSettings(s, "{\"statusBar\":2}", testEnv(), &needsResave));
  TEST_ASSERT_TRUE(needsResave);
}

// 5. Migration: pre-v2 textRenderMode (no V2/WeightOrder/NormalDark flags) is
// resolved through the weight-order palette and collapsed without crashing.
void test_migration_text_render_mode_prev2() {
  CrossPointSettings s;
  bool needsResave = false;
  TEST_ASSERT_TRUE(decodeSettings(s, "{\"textRenderMode\":1}", testEnv(), &needsResave));
  TEST_ASSERT_TRUE(needsResave);
  TEST_ASSERT_TRUE(s.textRenderMode < CrossPointSettings::TEXT_RENDER_MODE_COUNT);
}

// 6. Stable: a fully-current encoded doc triggers no migration on load.
void test_stable_no_resave_for_current_doc() {
  CrossPointSettings a;
  const std::string json = encodeToString(a);

  CrossPointSettings b;
  bool needsResave = true;
  TEST_ASSERT_TRUE(decodeSettings(b, json.c_str(), testEnv(), &needsResave));
  TEST_ASSERT_FALSE(needsResave);
}

// 7. Golden snapshot: encode a default CrossPointSettings and lock the exact
// serialized string. CAPTURE the actual output by running the test, paste it as
// the expected literal below.
// GOLDEN: locks the settings.json wire format + key order. Update deliberately.
void test_golden_default_snapshot() {
  CrossPointSettings a;
  const std::string json = encodeToString(a);
  const char* expected =
      "{\"homeLayout\":0,\"quoteScreenStyle\":0,\"sleepScreen\":0,\"sleepScreenCoverMode\":0,"
      "\"sleepScreenCoverFilter\":0,\"showSleepImageFilename\":0,\"showSleepFavoriteBadge\":0,\"statusBar\":2,"
      "\"statusBarEnabled\":1,\"statusBarShowBattery\":1,\"statusBarShowPageCounter\":0,\"statusBarPageCounterMode\":0,"
      "\"statusBarShowBookPercentage\":0,\"statusBarShowChapterPercentage\":0,\"statusBarShowBookBar\":0,"
      "\"statusBarShowChapterBar\":0,\"statusBarShowChapterTitle\":1,\"statusBarNoTitleTruncation\":0,"
      "\"statusBarTopLine\":0,\"statusBarBatteryPosition\":3,\"batteryPositionV2\":true,"
      "\"statusBarProgressTextPosition\":1,\"statusBarPageCounterPosition\":4,\"statusBarBookPercentagePosition\":4,"
      "\"statusBarChapterPercentagePosition\":4,\"statusBarBookBarPosition\":1,\"statusBarChapterBarPosition\":1,"
      "\"statusBarTitlePosition\":1,\"statusBarTextAlignment\":0,\"statusBarProgressStyle\":1,"
      "\"statusBarBarThickness\":0,"
      "\"statusBarShowPagesLeft\":0,\"statusBarPagesLeftPosition\":5,\"statusBarTitleContent\":0,"
      "\"statusBarShowChapterNumber\":0,\"statusBarChapterNumberPosition\":3,\"statusBarShowQuoteCount\":0,"
      "\"statusBarQuoteCountPosition\":5,\"statusBarShowFreeHeap\":0,\"statusBarFreeHeapPosition\":2,"
      "\"extraParagraphSpacingLevel\":2,\"extraParagraphSpacing\":true,\"wordSpacingPercent\":1,"
      "\"wordSpacingMidpoints\":true,\"firstLineIndentMode\":0,\"readerStyleMode\":0,\"textRenderMode\":0,"
      "\"textRenderModeV2\":true,\"textRenderModeWeightOrder\":true,\"textRenderModeNormalDark\":true,"
      "\"useFactoryLUT\":1,\"shortPwrBtn\":0,\"orientation\":0,\"tiltPageTurn\":0,\"statusBarClock\":0,"
      "\"clockUtcOffsetQ\":48,\"clockFormat\":0,\"clockHasBeenSynced\":0,\"sideButtonLayout\":0,\"frontButtonBack\":0,"
      "\"frontButtonConfirm\":1,\"frontButtonLeft\":2,\"frontButtonRight\":3,\"fontFamily\":1,\"fontSize\":4,"
      "\"lineSpacing\":1,\"lineSpacingPercent\":110,\"paragraphAlignment\":0,\"sleepTimeout\":2,\"showHiddenFiles\":0,"
      "\"randomBookOnBoot\":0,\"refreshFrequency\":3,\"screenMargin\":20,\"uniformMargins\":0,"
      "\"dynamicMargins\":0,\"screenMarginHorizontal\":20,\"screenMarginTop\":20,\"screenMarginBottom\":20,"
      "\"hideBatteryPercentage\":0,"
      "\"longPressChapterSkip\":1,\"hyphenationEnabled\":0,\"uiLanguage\":0,\"fadingFix\":0,\"embeddedStyle\":false,"
      "\"debugBorders\":0,\"highlightMode\":0,\"darkMode\":0,\"booksFolderOrder\":0,\"imageDither\":1}";
  TEST_ASSERT_EQUAL_STRING(expected, json.c_str());
}

// 8. Structural: every MECHANICAL field in the shared kFields table is BOTH
// written by encodeSettings and read back by decodeSettings. Iterate the table,
// set each field to a valid non-default value, round-trip it, and assert it
// survives. This is the standing guarantee that the v6.0.0 write-but-not-read
// regression class cannot recur for any table-driven field: if a row is dropped
// from encode (or decode), its round-trip fails here. Adding a new mechanical
// field to the table is automatically covered.
void test_structural_table_write_read_parity() {
  using crosspoint::persist::kFieldCount;
  using crosspoint::persist::kFields;
  TEST_ASSERT_TRUE(kFieldCount > 0);
  for (size_t i = 0; i < kFieldCount; ++i) {
    const auto& f = kFields[i];
    // A valid value that differs from the default: enums always have count >= 2,
    // clamped raws have maxRaw >= 1, and unclamped raws accept anything, so
    // flipping between 0 and 1 is always valid and never equals the default.
    const uint8_t testval = (f.deflt == 0) ? (uint8_t)1 : (uint8_t)0;

    CrossPointSettings a;  // defaults
    a.*(f.member) = testval;
    const std::string json = encodeToString(a);

    CrossPointSettings b;
    bool needsResave = false;
    TEST_ASSERT_TRUE(decodeSettings(b, json.c_str(), testEnv(), &needsResave));

    const uint8_t got = b.*(f.member);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "field '%s' (row %zu) not round-tripped: set %u, got %u", f.key, i,
                  (unsigned)testval, (unsigned)got);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(testval, got, msg);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_identity);
  RUN_TEST(test_drift_parity_granular_status_bar);
  RUN_TEST(test_migration_word_spacing_midpoint);
  RUN_TEST(test_migration_legacy_status_bar);
  RUN_TEST(test_migration_text_render_mode_prev2);
  RUN_TEST(test_stable_no_resave_for_current_doc);
  RUN_TEST(test_golden_default_snapshot);
  RUN_TEST(test_structural_table_write_read_parity);
  return UNITY_END();
}
