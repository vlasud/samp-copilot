#include "types.hpp"

#include <windows.h>

#include <chrono>

namespace gtabot {

std::int64_t NowMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

namespace {

// A strict UTF-8 walk. Strict on purpose: overlong forms and stray
// continuation bytes are exactly what a CP1251 string looks like, and
// accepting them would leave the bad bytes in place for nlohmann to choke on.
bool IsUtf8(const std::string& bytes) {
  std::size_t i = 0;
  while (i < bytes.size()) {
    const auto lead = static_cast<unsigned char>(bytes[i]);
    int extra = 0;
    unsigned int code = 0;
    if (lead < 0x80) { ++i; continue; }
    else if ((lead & 0xE0) == 0xC0) { extra = 1; code = lead & 0x1Fu; }
    else if ((lead & 0xF0) == 0xE0) { extra = 2; code = lead & 0x0Fu; }
    else if ((lead & 0xF8) == 0xF0) { extra = 3; code = lead & 0x07u; }
    else return false;

    if (i + extra >= bytes.size()) return false;
    for (int k = 1; k <= extra; ++k) {
      const auto next = static_cast<unsigned char>(bytes[i + k]);
      if ((next & 0xC0) != 0x80) return false;
      code = (code << 6) | (next & 0x3Fu);
    }
    // Overlong encodings, surrogates and out-of-range code points.
    if (extra == 1 && code < 0x80) return false;
    if (extra == 2 && code < 0x800) return false;
    if (extra == 3 && code < 0x10000) return false;
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    i += extra + 1;
  }
  return true;
}

}  // namespace

std::string ToUtf8(const std::string& bytes) {
  if (bytes.empty() || IsUtf8(bytes)) return bytes;

  constexpr UINT kWindows1251 = 1251;
  const int wide_length =
      MultiByteToWideChar(kWindows1251, 0, bytes.data(),
                          static_cast<int>(bytes.size()), nullptr, 0);
  if (wide_length <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(wide_length), wchar_t{});
  MultiByteToWideChar(kWindows1251, 0, bytes.data(),
                      static_cast<int>(bytes.size()), wide.data(), wide_length);

  const int utf8_length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                              wide_length, nullptr, 0, nullptr,
                                              nullptr);
  if (utf8_length <= 0) return {};
  std::string out(static_cast<std::size_t>(utf8_length), char{});
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, out.data(),
                      utf8_length, nullptr, nullptr);
  return out;
}

std::string ModuleDirectory() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self);
  char path[MAX_PATH] = {};
  GetModuleFileNameA(self, path, MAX_PATH);
  std::string full(path);
  const std::size_t slash = full.find_last_of("/\\");
  return slash == std::string::npos ? std::string{} : full.substr(0, slash + 1);
}

}  // namespace gtabot
