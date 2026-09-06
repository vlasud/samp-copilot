#pragma once
//
// The channel between the game thread and everyone else.
//
// Work that must touch game memory is posted here and executed inside the
// frame hook, so no other thread ever reads a structure while the game is
// mutating it.
//
#include <functional>

#include "types.hpp"

namespace gtabot::asi {

class Bridge {
 public:
  using Task = std::function<void()>;

  // Callable from any thread. Tasks run in posting order.
  static void PostToGameThread(Task task);

  // Game thread only, called from the frame hook. Bounded so a burst of
  // requests cannot turn into a frame spike.
  static void RunPending(std::size_t max_tasks);

  // The latest world state the game thread managed to build. Kept in a slot
  // rather than the outbox so the worker can keep reporting - with the age
  // attached - even while the game has stopped rendering and the game thread
  // is producing nothing at all.
  static void SetWorld(json world);
  static json GetWorld(std::int64_t* age_ms);
  // Just the age, without copying the snapshot - the overlay asks every frame.
  static std::int64_t world_age_ms();

  static std::size_t pending_tasks();
  // Tasks dropped because the queue was full - a stuck game thread, not a
  // slow one, and worth reporting rather than hiding.
  static std::size_t dropped_tasks();
};

}  // namespace gtabot::asi
