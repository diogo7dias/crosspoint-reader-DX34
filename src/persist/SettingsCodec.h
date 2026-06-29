#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <functional>
#include <string>
class CrossPointSettings;
namespace crosspoint {
namespace persist {
struct SettingsEnv {
  std::function<uint8_t()> languageCount;                             // i18n getLanguageCount()
  std::function<std::string(const std::string&)> obfuscate;           // obfuscateToBase64 (returns content)
  std::function<std::string(const std::string&, bool*)> deobfuscate;  // deobfuscateFromBase64
  std::function<void(bool)> applyFactoryLut;                          // setBitmapHelpersUseFactoryLUT
};
void encodeSettings(const CrossPointSettings& s, JsonDocument& doc, const SettingsEnv& env);
bool decodeSettings(CrossPointSettings& s, const char* json, const SettingsEnv& env, bool* needsResave);
}  // namespace persist
}  // namespace crosspoint
