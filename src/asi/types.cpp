#include "types.hpp"

#include <windows.h>

#include <chrono>

namespace gtabot {

std::int64_t NowMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
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
