#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <server/session.hpp>
#include <server/uinput.hpp>
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
  // Channel count the running audio pipeline was built with (the pulse sink is only recreated
  // on fresh launch, so a resume with a different count needs the pipeline rebuilt).
  int audio_channels() const { return audio_channels_; }
  void rebuild_audio(const session::StreamSession &s);

  void mouse_move_rel(double dx, double dy);
  void mouse_move_abs(double x, double y);
  void mouse_button(unsigned int linux_button, bool pressed);
  void mouse_axis(double x, double y); // high-res scroll: (h, v)
  void keyboard_key(unsigned int linux_key, bool pressed);
  // Controller hotplug (Wolf model): a map of virtual pads keyed by the client's controller_number.
  // gamepad_arrival cold-creates a pad on CONTROLLER_ARRIVAL; gamepad_update creates lazily for old
  // clients and, per the CONTROLLER_MULTI active_gamepad_mask, tears a pad down when its bit clears.
  // Pad 0 is also created at session start so the common single-controller case is present before
  // Steam/SDL's initial scan.
  void gamepad_arrival(int controller_number);
  void gamepad_update(int controller_number, std::uint16_t active_gamepad_mask,
                      std::uint32_t button_flags, std::uint8_t left_trigger,
                      std::uint8_t right_trigger, short left_x, short left_y, short right_x,
                      short right_y);
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
  int audio_channels_ = 2;
  std::atomic<std::uint64_t> video_buffers_{0};
  std::atomic<std::uint64_t> audio_buffers_{0};
  std::shared_ptr<std::atomic<pid_t>> app_pid_ = std::make_shared<std::atomic<pid_t>>(-1);
  // controller_number -> virtual pad. Guarded by gamepad_mtx_ (control thread mutates on hotplug).
  std::mutex gamepad_mtx_;
  std::map<int, std::unique_ptr<input::VirtualGamepad>> gamepads_;
  // Leaked: detached listener threads outlive the pipeline; kept only to re-target on resume.
  UDPSink *video_sink_ = nullptr;
  UDPSink *audio_sink_ = nullptr;
  friend struct UDPSink;
};

} // namespace media
