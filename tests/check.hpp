#pragma once
//
// The smallest harness that still says what went wrong.
//
#include <cstdio>
#include <string>

namespace check {

inline int g_failed = 0;
inline int g_ran = 0;

inline void Is(const std::string& got, const std::string& want,
               const char* what) {
  ++g_ran;
  if (got == want) return;
  ++g_failed;
  std::printf("  FAIL %s\n    got  \"%s\"\n    want \"%s\"\n", what,
              got.c_str(), want.c_str());
}

inline void Is(int got, int want, const char* what) {
  ++g_ran;
  if (got == want) return;
  ++g_failed;
  std::printf("  FAIL %s\n    got  %d\n    want %d\n", what, got, want);
}

inline void True(bool got, const char* what) {
  ++g_ran;
  if (got) return;
  ++g_failed;
  std::printf("  FAIL %s\n", what);
}

}  // namespace check
