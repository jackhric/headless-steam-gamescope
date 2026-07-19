#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace input {

// Mirrors gamescope's internal Xwayland cursor into the parent compositor.
// Steam's gamepadui guide-chord cursor is injected via XTest/libei entirely
// inside nested gamescope: it moves gamescope's internal cursor, but the
// visible cursor is drawn by the parent compositor at the parent pointer's
// position, so chord motion is invisible. This polls gamescope's X cursor and
// re-injects it as absolute parent pointer motion so the two stay in step.
class PointerSync {
public:
  using InjectFn = std::function<void(double x, double y)>;

  explicit PointerSync(InjectFn inject_abs);
  ~PointerSync();
  PointerSync(const PointerSync &) = delete;
  PointerSync &operator=(const PointerSync &) = delete;

  // Real Moonlight mouse input arrived; sync is suppressed for a short window so
  // client-driven motion doesn't round-trip through the poll.
  void note_client_mouse();

private:
  void run();

  InjectFn inject_;
  std::atomic<bool> stop_{false};
  std::atomic<std::int64_t> last_client_ms_{0};
  std::thread thread_;
};

} // namespace input
