#include <server/media.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <helpers/logger.hpp>
#include <mutex>
#include <netinet/in.h>
#include <server/launcher.hpp>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace media {

namespace {

std::string color_range_str(session::ColorRange r) {
  return r == session::ColorRange::JPEG ? "jpeg" : "mpeg2";
}
std::string color_space_str(session::ColorSpace s) {
  switch (s) {
  case session::ColorSpace::BT709: return "bt709";
  case session::ColorSpace::BT2020: return "bt2020";
  default: return "bt601";
  }
}

std::string channel_mask(int ch) {
  switch (ch) {
  case 2: return "0x3";
  case 6: return "0x3f";
  case 8: return "0xc3f";
  default: return "0x3";
  }
}

} // namespace

struct UDPSink {
  int fd = -1;
  unsigned short listen_port = 0;
  std::string label;
  std::string client_ip;
  std::atomic<bool> have_client{false};
  sockaddr_in dest{};
  std::atomic<std::uint64_t> *counter = nullptr;
  std::atomic<bool> *running = nullptr;

  void send(const guint8 *data, gsize size) {
    if (!have_client.load())
      return;
    ::sendto(fd, data, size, 0, (sockaddr *)&dest, sizeof(dest));
  }

};

namespace {

GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
  auto *sink = static_cast<UDPSink *>(user_data);
  GstSample *sample = gst_app_sink_pull_sample(appsink);
  if (!sample)
    return GST_FLOW_ERROR;

  if (GstBufferList *list = gst_sample_get_buffer_list(sample)) {
    guint n = gst_buffer_list_length(list);
    for (guint i = 0; i < n; i++) {
      GstBuffer *buf = gst_buffer_list_get(list, i);
      GstMapInfo map;
      if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
        sink->send(map.data, map.size);
        gst_buffer_unmap(buf, &map);
        ++(*sink->counter);
      }
    }
  } else if (GstBuffer *buf = gst_sample_get_buffer(sample)) {
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
      sink->send(map.data, map.size);
      gst_buffer_unmap(buf, &map);
      ++(*sink->counter);
    }
  }
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

int start_udp_listener(UDPSink *sink) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    logs::log(logs::error, "[MEDIA] socket() failed: {}", std::strerror(errno));
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = INADDR_ANY;
  local.sin_port = htons(sink->listen_port);
  if (::bind(fd, (sockaddr *)&local, sizeof(local)) < 0) {
    logs::log(logs::error, "[MEDIA] bind :{} failed: {}", sink->listen_port, std::strerror(errno));
    ::close(fd);
    return -1;
  }
  sink->fd = fd;

  std::thread([sink]() {
    char buf[2048];
    while (sink->running->load()) {
      sockaddr_in from{};
      socklen_t flen = sizeof(from);
      ssize_t n = ::recvfrom(sink->fd, buf, sizeof(buf), 0, (sockaddr *)&from, &flen);
      if (n <= 0)
        continue;
      // Follow the ping source so a client that re-binds a new ephemeral port on resume keeps
      // receiving RTP without a pipeline teardown.
      bool first = !sink->have_client.load();
      bool moved = !first && (sink->dest.sin_addr.s_addr != from.sin_addr.s_addr ||
                              sink->dest.sin_port != from.sin_port);
      if (first || moved) {
        sink->dest = from;
        sink->have_client = true;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        logs::log(logs::info, "[MEDIA] RTP client {} on :{} -> {}:{}",
                  first ? "discovered" : "re-targeted", sink->listen_port, ip,
                  ntohs(from.sin_port));
      }
    }
  }).detach();
  return fd;
}

// waylanddisplaysrc posts a "wayland.src" bus message with WAYLAND_DISPLAY once the embedded
// compositor is up; launch the app into it (or STEAM_STREAM_TEST_CLIENT stand-in) so frames flow.
void watch_wayland_and_launch(GstElement *pipeline,
                              const std::shared_ptr<session::StreamSession> &session,
                              std::shared_ptr<std::atomic<pid_t>> app_pid) {
  const char *test_cmd = std::getenv("STEAM_STREAM_TEST_CLIENT");
  std::string command = test_cmd ? test_cmd : "";
  std::thread([pipeline, command, session, app_pid]() {
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    for (int i = 0; i < 200; i++) { // ~20s window
      GstMessage *msg =
          gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND, GST_MESSAGE_APPLICATION);
      if (!msg)
        continue;
      const GstStructure *st = gst_message_get_structure(msg);
      if (st && gst_structure_has_name(st, "wayland.src")) {
        const char *disp = gst_structure_get_string(st, "WAYLAND_DISPLAY");
        if (disp) {
          if (std::getenv("STEAM_STREAM_INPUT_SELFTEST")) {
            GstElement *ws = gst_bin_get_by_name(GST_BIN(pipeline), "wolf_wayland_source");
            if (ws) {
              auto fire = [&](const char *label, GstStructure *s2) {
                gboolean ok =
                    gst_element_send_event(ws, gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, s2));
                logs::log(logs::info, "[INPUT-SELFTEST] {} accepted={}", label, ok ? "yes" : "no");
              };
              fire("MouseMoveAbsolute", gst_structure_new("MouseMoveAbsolute", "pointer_x",
                                                          G_TYPE_DOUBLE, 100.0, "pointer_y",
                                                          G_TYPE_DOUBLE, 100.0, NULL));
              fire("MouseMoveRelative", gst_structure_new("MouseMoveRelative", "pointer_x",
                                                          G_TYPE_DOUBLE, 5.0, "pointer_y",
                                                          G_TYPE_DOUBLE, 5.0, NULL));
              fire("MouseButton", gst_structure_new("MouseButton", "button", G_TYPE_UINT, 0x110u,
                                                    "pressed", G_TYPE_BOOLEAN, TRUE, NULL));
              fire("MouseAxis", gst_structure_new("MouseAxis", "x", G_TYPE_DOUBLE, 0.0, "y",
                                                  G_TYPE_DOUBLE, -1.0, NULL));
              fire("KeyboardKey", gst_structure_new("KeyboardKey", "key", G_TYPE_UINT, 30u,
                                                    "pressed", G_TYPE_BOOLEAN, TRUE, NULL));
              gst_object_unref(ws);
            }
          }
          if (!command.empty()) {
            logs::log(logs::info,
                      "[MEDIA] compositor ready (WAYLAND_DISPLAY={}); launching test client: {}",
                      disp, command);
            setenv("WAYLAND_DISPLAY", disp, 1);
            std::string full = command + " &";
            if (std::system(full.c_str()) != 0)
              logs::log(logs::warning, "[MEDIA] test client launch returned non-zero");
          } else {
            logs::log(logs::info, "[MEDIA] compositor ready (WAYLAND_DISPLAY={}); launching app",
                      disp);
            pid_t pid = launcher::launch_app(*session, disp);
            if (pid > 0)
              app_pid->store(pid);
          }
          gst_message_unref(msg);
          break;
        }
      }
      gst_message_unref(msg);
    }
    gst_object_unref(bus);
  }).detach();
}

GstElement *launch(const std::string &pipeline, const char *sink_name, UDPSink *sink) {
  GError *err = nullptr;
  GstElement *pl = gst_parse_launch(pipeline.c_str(), &err);
  if (!pl || err) {
    logs::log(logs::error, "[MEDIA] failed to construct pipeline: {}",
              err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    if (pl)
      gst_object_unref(pl);
    return nullptr;
  }
  GstElement *as = gst_bin_get_by_name(GST_BIN(pl), sink_name);
  if (as) {
    g_object_set(as, "emit-signals", FALSE, NULL);
    GstAppSinkCallbacks cbs = {nullptr};
    cbs.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(as), &cbs, sink, nullptr);
    gst_object_unref(as);
  } else {
    logs::log(logs::warning, "[MEDIA] appsink '{}' not found in pipeline", sink_name);
  }
  if (gst_element_set_state(pl, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    logs::log(logs::error, "[MEDIA] pipeline failed to reach PLAYING");
    gst_object_unref(pl);
    return nullptr;
  }
  return pl;
}

} // namespace

long encoder_vbv_kbit(long bitrate_kbps, int fps) {
  // Size VBV against a 60fps floor so high refresh rates (e.g. 1080p240) don't starve the buffer
  // and crush detailed frames under cbr-ld-hq.
  int fps_floor = fps > 60 ? 60 : (fps > 0 ? fps : 60);
  return bitrate_kbps / fps_floor;
}

std::string build_video_pipeline(const session::StreamSession &s, const std::string &render_node) {
  long vbv = encoder_vbv_kbit(s.video.bitrate_kbps, s.video.fps);
  return fmt::format(
      "waylanddisplaysrc name=wolf_wayland_source render-node={node} ! "
      "video/x-raw,format=RGBx,width={w},height={h},framerate={fps}/1 ! "
      "cudaupload ! cudaconvertscale add-borders=true ! "
      "video/x-raw(memory:CUDAMemory),width={w},height={h},chroma-site={crange},format=NV12,"
      "colorimetry={cspace},pixel-aspect-ratio=1/1 ! "
      "nvh264enc name=video_encoder preset=low-latency-hq zerolatency=true gop-size=0 rc-mode=cbr-ld-hq "
      "bitrate={br} vbv-buffer-size={vbv} aud=false ! "
      "h264parse ! video/x-h264,profile=main,stream-format=byte-stream ! "
      "rtpmoonlightpay_video name=moonlight_pay payload_size={ps} fec_percentage={fec} "
      "min_required_fec_packets={minfec} ! "
      "appsink name=video_sink sync=false max-buffers=1 drop=true",
      fmt::arg("node", render_node), fmt::arg("w", s.video.width), fmt::arg("h", s.video.height),
      fmt::arg("fps", s.video.fps), fmt::arg("crange", color_range_str(s.video.color_range)),
      fmt::arg("cspace", color_space_str(s.video.color_space)), fmt::arg("br", s.video.bitrate_kbps),
      fmt::arg("vbv", vbv), fmt::arg("ps", s.video.packet_size), fmt::arg("fec", s.video.fec_percentage),
      fmt::arg("minfec", s.video.min_required_fec_packets));
}

std::string build_audio_pipeline(const session::StreamSession &s, bool use_test_src) {
  std::string src =
      use_test_src
          ? "audiotestsrc is-live=true wave=sine"
          : fmt::format("pulsesrc device=\"{}\" server=\"{}\"", "steam-stream.monitor",
                        std::getenv("PULSE_SERVER") ? std::getenv("PULSE_SERVER") : "");
  return fmt::format(
      "{src} ! audiorate ! audioconvert ! audioresample ! "
      "audio/x-raw,channels={ch},channel-mask=(bitmask){mask},rate=48000 ! "
      "opusenc bitrate={br} bitrate-type=cbr frame-size={pd} bandwidth=fullband "
      "audio-type=restricted-lowdelay max-payload-size=1400 ! "
      "rtpmoonlightpay_audio name=moonlight_pay packet_duration={pd} encrypt={enc} "
      "aes_key=\"{key}\" aes_iv=\"{iv}\" ! "
      "appsink name=audio_sink sync=false",
      fmt::arg("src", src), fmt::arg("ch", s.audio.channels), fmt::arg("mask", channel_mask(s.audio.channels)),
      fmt::arg("br", s.audio.bitrate), fmt::arg("pd", s.audio.packet_duration),
      fmt::arg("enc", s.audio.encrypt ? "true" : "false"), fmt::arg("key", s.aes_key),
      fmt::arg("iv", s.aes_iv));
}

void MediaSession::global_init() {
  if (!gst_is_initialized())
    gst_init(nullptr, nullptr);
}

std::shared_ptr<MediaSession> MediaSession::start(const std::shared_ptr<session::StreamSession> &s,
                                                 const std::string &render_node) {
  global_init();
  auto ms = std::shared_ptr<MediaSession>(new MediaSession());
  ms->session_ = s;

  static std::atomic<bool> *running = new std::atomic<bool>(true); // process-lifetime
  bool audio_test = std::getenv("STEAM_STREAM_AUDIO_TEST") != nullptr;

  // Video
  auto *vsink = new UDPSink(); // leaked: lives as long as the pipeline callback
  vsink->listen_port = s->video_stream_port;
  vsink->client_ip = s->client_ip;
  vsink->counter = &ms->video_buffers_;
  vsink->label = "video";
  vsink->running = running;
  ms->video_sink_ = vsink;
  start_udp_listener(vsink);
  auto vpipe = build_video_pipeline(*s, render_node);
  logs::log(logs::info, "[MEDIA] starting video pipeline:\n{}", vpipe);
  ms->video_pipeline_ = launch(vpipe, "video_sink", vsink);
  if (ms->video_pipeline_) {
    ms->wayland_src_ = gst_bin_get_by_name(GST_BIN(ms->video_pipeline_), "wolf_wayland_source");
    if (!ms->wayland_src_)
      logs::log(logs::warning, "[MEDIA] wolf_wayland_source not found -- input injection disabled");
    watch_wayland_and_launch(ms->video_pipeline_, s, ms->app_pid_);
  }

  // Audio
  auto *asink = new UDPSink();
  asink->listen_port = s->audio_stream_port;
  asink->client_ip = s->client_ip;
  asink->counter = &ms->audio_buffers_;
  asink->label = "audio";
  asink->running = running;
  ms->audio_sink_ = asink;
  start_udp_listener(asink);
  auto apipe = build_audio_pipeline(*s, audio_test);
  logs::log(logs::info, "[MEDIA] starting audio pipeline:\n{}", apipe);
  ms->audio_pipeline_ = launch(apipe, "audio_sink", asink);

  if (!ms->video_pipeline_)
    logs::log(logs::error, "[MEDIA] video pipeline did not start");
  return ms;
}

void MediaSession::send_wayland_event(GstStructure *msg) {
  if (!wayland_src_) {
    gst_structure_free(msg);
    return;
  }
  gst_element_send_event(wayland_src_, gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, msg));
}

void MediaSession::mouse_move_rel(double dx, double dy) {
  send_wayland_event(gst_structure_new("MouseMoveRelative", "pointer_x", G_TYPE_DOUBLE, dx,
                                       "pointer_y", G_TYPE_DOUBLE, dy, NULL));
}

void MediaSession::mouse_move_abs(double x, double y) {
  send_wayland_event(gst_structure_new("MouseMoveAbsolute", "pointer_x", G_TYPE_DOUBLE, x,
                                       "pointer_y", G_TYPE_DOUBLE, y, NULL));
}

void MediaSession::mouse_button(unsigned int linux_button, bool pressed) {
  send_wayland_event(gst_structure_new("MouseButton", "button", G_TYPE_UINT, linux_button,
                                       "pressed", G_TYPE_BOOLEAN, pressed, NULL));
}

void MediaSession::mouse_axis(double x, double y) {
  send_wayland_event(
      gst_structure_new("MouseAxis", "x", G_TYPE_DOUBLE, x, "y", G_TYPE_DOUBLE, y, NULL));
}

void MediaSession::keyboard_key(unsigned int linux_key, bool pressed) {
  send_wayland_event(gst_structure_new("KeyboardKey", "key", G_TYPE_UINT, linux_key, "pressed",
                                       G_TYPE_BOOLEAN, pressed, NULL));
}

void MediaSession::force_idr() {
  if (video_pipeline_) {
    logs::log(logs::debug, "[MEDIA] force_idr (GstForceKeyUnit) on video pipeline");
    gst_element_send_event(
        video_pipeline_,
        gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM,
                             gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN,
                                               TRUE, NULL)));
  }
}

void MediaSession::update_bitrate(long bitrate_kbps, int fps) {
  if (!video_pipeline_ || bitrate_kbps <= 0)
    return;
  GstElement *enc = gst_bin_get_by_name(GST_BIN(video_pipeline_), "video_encoder");
  if (!enc) {
    logs::log(logs::warning, "[MEDIA] update_bitrate: 'video_encoder' not found -- bitrate "
                             "change ignored (client may stay blocky)");
    return;
  }
  long vbv = encoder_vbv_kbit(bitrate_kbps, fps);
  guint cur_br = 0;
  g_object_get(enc, "bitrate", &cur_br, NULL);
  if (static_cast<long>(cur_br) == bitrate_kbps) {
    gst_object_unref(enc);
    return;
  }
  g_object_set(enc, "bitrate", static_cast<guint>(bitrate_kbps), "vbv-buffer-size",
               static_cast<guint>(vbv), NULL);
  gst_object_unref(enc);
  logs::log(logs::info, "[MEDIA] encoder bitrate updated {}->{}kbps (vbv={}kbit) for resume", cur_br,
            bitrate_kbps, vbv);
}

void MediaSession::retarget() {
  for (UDPSink *s : {video_sink_, audio_sink_}) {
    if (!s)
      continue;
    // Drop the stale destination so the listener re-discovers the reconnected client from its
    // next ping.
    s->have_client = false;
    logs::log(logs::info, "[MEDIA] {} retarget: awaiting reconnected client RTP ping on :{}",
              s->label, s->listen_port);
  }

  // AES key rotates on resume; the audio payloader baked the old key at build, so re-push the
  // new key to the live element or the client can't decrypt.
  if (audio_pipeline_ && session_ && !session_->aes_key.empty()) {
    GstElement *pay = gst_bin_get_by_name(GST_BIN(audio_pipeline_), "moonlight_pay");
    if (pay) {
      std::string key, iv;
      {
        std::lock_guard<std::mutex> lk(*session_->mtx);
        key = session_->aes_key;
        iv = session_->aes_iv;
      }
      g_object_set(pay, "aes_key", key.c_str(), "aes_iv", iv.c_str(), NULL);
      gst_object_unref(pay);
      logs::log(logs::info, "[MEDIA] audio payloader AES key refreshed for resume (rikeyid={})", iv);
    } else {
      logs::log(logs::warning, "[MEDIA] retarget: audio payloader 'moonlight_pay' not found -- "
                               "audio may fail to decrypt after resume");
    }
  }
}

void MediaSession::stop() {
  // The abstract X11 socket (@/tmp/.X11-unix/X0) is only freed on process exit, so BLOCK until
  // the app's process group is gone -- else the next session hits "Address already in use".
  if (pid_t pid = app_pid_->exchange(-1); pid > 0) {
    logs::log(logs::info, "[MEDIA] stopping launched app (pgid {})", pid);
    ::kill(-pid, SIGTERM);
    auto group_alive = [pid]() { return ::kill(-pid, 0) == 0 || errno != ESRCH; };
    bool dead = false;
    for (int i = 0; i < 50; i++) { // ~5s
      if (!group_alive()) { dead = true; break; }
      ::usleep(100 * 1000);
    }
    if (!dead) {
      logs::log(logs::warning, "[MEDIA] app group {} ignored SIGTERM -- SIGKILL", pid);
      ::kill(-pid, SIGKILL);
      for (int i = 0; i < 20; i++) { // ~2s
        if (!group_alive()) { dead = true; break; }
        ::usleep(100 * 1000);
      }
    }
    logs::log(logs::info, "[MEDIA] app group {} {}", pid, dead ? "reaped" : "still lingering");
  }
  if (wayland_src_) {
    gst_object_unref(wayland_src_);
    wayland_src_ = nullptr;
  }
  if (video_pipeline_) {
    gst_element_set_state(video_pipeline_, GST_STATE_NULL);
    gst_object_unref(video_pipeline_);
    video_pipeline_ = nullptr;
  }
  if (audio_pipeline_) {
    gst_element_set_state(audio_pipeline_, GST_STATE_NULL);
    gst_object_unref(audio_pipeline_);
    audio_pipeline_ = nullptr;
  }
}

MediaSession::~MediaSession() { stop(); }

} // namespace media
