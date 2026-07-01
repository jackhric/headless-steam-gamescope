#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <server/session.hpp>

namespace media {
class MediaSession;
}

namespace control {

class ControlServer {
public:
  ControlServer(int port, std::shared_ptr<session::SessionRegistry> registry);
  ~ControlServer();

  void set_idr_callback(std::function<void(std::size_t /*session_id*/)> cb);

  // Accessor returns nullptr before PLAY / between sessions.
  void set_media_accessor(std::function<std::shared_ptr<media::MediaSession>()> cb);

  void run();
  void stop();

  static bool global_init(); // enet_initialize(); call once at startup.

private:
  int port_;
  std::shared_ptr<session::SessionRegistry> registry_;
  std::function<void(std::size_t)> on_idr_;
  std::function<std::shared_ptr<media::MediaSession>()> get_media_;
  std::atomic<bool> running_{false};
};

} // namespace control
