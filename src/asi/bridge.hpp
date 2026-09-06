#pragma once
//
// The two-way channel between the game thread and the IO thread.
//
// Work that must touch game memory is posted here and executed inside the
// frame hook. Anything the game thread produces goes into the outbox and is
// written to the pipe by the worker, so the render thread never blocks on IO.
//
#include <functional>
#include <vector>

#include "common/protocol.hpp"

namespace gtabot::asi {

class Bridge {
 public:
  using Task = std::function<void()>;

  // Callable from any thread. Tasks run in posting order.
  static void PostToGameThread(Task task);

  // Game thread only, called from the frame hook. Bounded so a burst of
  // requests cannot turn into a frame spike.
  static void RunPending(std::size_t max_tasks);

  // Game thread produces, worker consumes.
  static void Publish(proto::Envelope envelope);
  static std::vector<proto::Envelope> DrainOutbox();

  static std::size_t pending_tasks();
  // Tasks dropped because the queue was full - a stuck game thread, not a
  // slow one, and worth reporting rather than hiding.
  static std::size_t dropped_tasks();
};

}  // namespace gtabot::asi
