// Minimal host shadow of Arduino's String, backed by std::string. Only used so
// HalStorage's String-returning/-taking signatures are a complete type; the
// ZIP slice exercises the std::string / const char* overloads.
#pragma once

#include <string>

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& s) : std::string(s) {}
  String(const char* s) : std::string(s ? s : "") {}
};
