#include "I18n.h"

#include "I18nStrings.h"

using namespace i18n_strings;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  const char* const* strings = getStringArray(currentLanguage_);
  return strings[index];
}

void I18n::setLanguage(Language lang) {
  if (static_cast<size_t>(lang) >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::ENGLISH;
  }
  currentLanguage_ = lang;
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::ENGLISH;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
