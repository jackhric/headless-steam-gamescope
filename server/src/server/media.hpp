#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <server/session.hpp>
#include <string>
#include <sys/types.h>

typedef struct _GstElement GstElement;
typedef struct _GstStructure GstStructure;

namespace media {

struct UDPSink;

std::string build_video_pipeline(const session::StreamSession &s, const std::string &render_node);
// use_test_src swaps pulsesrc for audiotestsrc (offline test).
std::string build_audio_pipeline(const session::StreamSession &s, bool use_test_src);

class MediaSession {
public:
  static std::shared_ptr<MediaSession> start(const std::shared_ptr<session::StreamSession> &s,
                                             const std::string &render_node);
  ~MediaSession();

  void stop();
  void force_idr();

  std::size_t session_id() const { return session_ ? session_->session_id : 0; }
  bool is_active() const { return video_pipeline_ != nullptr; }
  pid_t app_pid() const { return app_pid_->load(); }
  void retarget();
  void update_bitrate(long bitrate_kbps, int fps);

  void mouse_move_rel(double dx, double dy);
  void mouse_move_abs(double x, double y);
  void mouse_button(unsigned int linux_button, bool pressed);
  void mouse_axis(double x, double y); // high-res scroll: (h, v)
  void keyboard_key(unsigned int linux_key, bool pressed);
  bool has_wayland_src() const { return wayland_src_ != nullptr; }

  std::uint64_t video_buffers() const { return video_buffers_; }
  std::uint64_t audio_buffers() const { return audio_buffers_; }

  static void global_init();

private:
  void send_wayland_event(GstStructure *msg); // takes ownership of msg

  std::shared_ptr<session::StreamSession> session_;
  GstElement *video_pipeline_ = nullptr;
  GstElement *wayland_src_ = nullptr; // borrowed-from-pipeline ref
  GstElement *audio_pipeline_ = nullptr;
  std::atomic<std::uint64_t> video_buffers_{0};
  std::atomic<std::uint64_t> audio_buffers_{0};
  std::shared_ptr<std::atomic<pid_t>> app_pid_ = std::make_shared<std::atomic<pid_t>>(-1);
  // Leaked: detached listener threads outlive the pipeline; kept only to re-target on resume.
  UDPSink *video_sink_ = nullptr;
  UDPSink *audio_sink_ = nullptr;
  friend struct UDPSink;
};

} // namespace media
