#include "types.hpp"

#include <chrono>

namespace gtabot {

std::int64_t NowMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

}  // namespace gtabot
