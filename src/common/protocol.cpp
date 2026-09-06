#include "protocol.hpp"

#include <chrono>
#include <utility>

namespace gtabot::proto {

std::int64_t NowMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

Envelope Make(std::string type, json payload, std::uint64_t id) {
  Envelope e;
  e.type    = std::move(type);
  e.payload = std::move(payload);
  e.id      = id;
  e.ts      = NowMillis();
  return e;
}

}  // namespace gtabot::proto
