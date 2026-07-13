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
#include <server/encoder_config.hpp>
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

  // One-shot: the first buffer signals the pipeline is flowing (also the marker verify-media.sh asserts on).
  void note_first(std::uint64_t n) {
    if (n == 1)
      logs::log(logs::info, "[MEDIA] {} appsink produced first RTP buffer{}", label,
                have_client.load() ? " (sending to client)" : " (no client yet, dropping)");
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
        sink->note_first(++(*sink->counter));
      }
    }
  } else if (GstBuffer *buf = gst_sample_get_buffer(sample)) {
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
      sink->send(map.data, map.size);
      gst_buffer_unmap(buf, &map);
      sink->note_first(++(*sink->counter));
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
                              std::shared_ptr<std::atomic<pid_t>> app_pid,
                              const std::string &pulse_sink) {
  const char *test_cmd = std::getenv("STEAM_STREAM_TEST_CLIENT");
  std::string command = test_cmd ? test_cmd : "";
  std::thread([pipeline, command, session, app_pid, pulse_sink]() {
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
            pid_t pid = launcher::launch_app(*session, disp, pulse_sink);
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

// --- encoder diagnostics (gated by STEAM_STREAM_ENCODER_DIAG) ----------------
// Prove the negotiated bitrate reaches the encoder and measure what actually comes out,
// so we can tell encoder starvation (VBV) apart from downstream drops/network loss.

bool encoder_diag_enabled() { return std::getenv("STEAM_STREAM_ENCODER_DIAG") != nullptr; }

void log_encoder_readback(GstElement *pipeline) {
  GstElement *enc = gst_bin_get_by_name(GST_BIN(pipeline), "video_encoder");
  if (!enc) {
    logs::log(logs::warning, "[MEDIA-DIAG] video_encoder not found for read-back");
    return;
  }
  guint bitrate = 0, vbv = 0;
  GParamSpec *ps_br = g_object_class_find_property(G_OBJECT_GET_CLASS(enc), "bitrate");
  GParamSpec *ps_vbv = g_object_class_find_property(G_OBJECT_GET_CLASS(enc), "vbv-buffer-size");
  if (ps_br)
    g_object_get(enc, "bitrate", &bitrate, NULL);
  if (ps_vbv)
    g_object_get(enc, "vbv-buffer-size", &vbv, NULL);
  logs::log(logs::info, "[MEDIA-DIAG] encoder live bitrate={}kbit/s vbv-buffer-size={}kbit", bitrate,
            vbv);
  gst_object_unref(enc);
}

// Per-encoder-src accounting: bytes and buffers over rolling ~1s windows -> Mbps out.
struct EncoderRateProbe {
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> buffers{0};
  std::atomic<std::int64_t> window_start_ns{0};
};

GstPadProbeReturn encoder_rate_cb(GstPad *, GstPadProbeInfo *info, gpointer user_data) {
  auto *p = static_cast<EncoderRateProbe *>(user_data);
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf)
    return GST_PAD_PROBE_OK;
  p->bytes.fetch_add(gst_buffer_get_size(buf));
  p->buffers.fetch_add(1);
  gint64 now = g_get_monotonic_time() * 1000; // us -> ns
  gint64 start = p->window_start_ns.load();
  if (start == 0) {
    p->window_start_ns.store(now);
    return GST_PAD_PROBE_OK;
  }
  if (now - start >= 1'000'000'000) {
    double secs = (now - start) / 1e9;
    std::uint64_t b = p->bytes.exchange(0);
    std::uint64_t frames = p->buffers.exchange(0);
    p->window_start_ns.store(now);
    logs::log(logs::info, "[MEDIA-DIAG] encoder out {:.2f} Mbps ({} buffers/{:.2f}s)",
              (b * 8.0) / secs / 1e6, frames, secs);
  }
  return GST_PAD_PROBE_OK;
}

void attach_encoder_rate_probe(GstElement *pipeline) {
  GstElement *enc = gst_bin_get_by_name(GST_BIN(pipeline), "video_encoder");
  if (!enc) {
    logs::log(logs::warning, "[MEDIA-DIAG] video_encoder not found for rate probe");
    return;
  }
  GstPad *src = gst_element_get_static_pad(enc, "src");
  if (src) {
    auto *probe = new EncoderRateProbe(); // leaked: lives for the pipeline's lifetime
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, encoder_rate_cb, probe, nullptr);
    gst_object_unref(src);
    logs::log(logs::info, "[MEDIA-DIAG] encoder output rate probe attached");
  }
  gst_object_unref(enc);
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
  if (encoder_diag_enabled() && gst_bin_get_by_name(GST_BIN(pl), "video_encoder")) {
    log_encoder_readback(pl);
    attach_encoder_rate_probe(pl);
  }
  return pl;
}

} // namespace

long encoder_vbv_kbit(long bitrate_kbps, int fps) {
  // VBV is in kbits; bitrate_kbps is kbit/s, so bitrate_kbps/N == N-th-of-a-second of budget.
  // A 1-frame buffer (the old bitrate/fps) starves detailed motion frames under cbr-ld-hq,
  // producing blocky-in-motion / sharp-when-still. STEAM_STREAM_VBV_MS sets the HRD window in
  // milliseconds (default 500ms) so the encoder can spend on complexity peaks without adding
  // meaningful latency at CBR. Set a small value (e.g. 16) to restore the tight, old behaviour.
  (void)fps;
  long ms = 500;
  if (const char *e = std::getenv("STEAM_STREAM_VBV_MS")) {
    long parsed = std::atol(e);
    if (parsed > 0)
      ms = parsed;
  }
  long vbv = bitrate_kbps * ms / 1000;
  return vbv > 0 ? vbv : bitrate_kbps; // never zero (0 == NVENC default = unbounded)
}

std::string build_video_pipeline(const session::StreamSession &s, const std::string &render_node) {
  // Load config fresh so web-UI edits to encoders.toml apply on the next stream start.
  auto cfg = encoder_config::load_or_seed();
  auto encoder = encoder_config::select_encoder(cfg, s.video.format);
  if (!encoder) {
    logs::log(logs::error, "[MEDIA] no usable encoder for requested codec; cannot build pipeline");
    return {};
  }

  bool zero_copy_env = std::getenv("WOLF_USE_ZERO_COPY") &&
                       std::string(std::getenv("WOLF_USE_ZERO_COPY")) != "FALSE" &&
                       std::string(std::getenv("WOLF_USE_ZERO_COPY")) != "false";

  // Zero-copy needs BOTH the CUDAMemory capture head and the encoder's zero-copy params; they
  // must switch together (a CUDAMemory source with a cudaupload param block, or vice-versa,
  // won't link). Only engage when the config provides both, else use the RGBx source + params.
  bool zero_copy = zero_copy_env && cfg.video.default_source_zero_copy.has_value() &&
                   encoder->video_params_zero_copy.has_value();
  const std::string &source = zero_copy ? *cfg.video.default_source_zero_copy
                                        : cfg.video.default_source;
  const std::string &video_params =
      zero_copy ? *encoder->video_params_zero_copy : encoder->video_params;
  logs::log(logs::info, "[MEDIA] video path: {} ({})", zero_copy ? "zero-copy CUDAMemory" : "RGBx",
            encoder->plugin_name);

  auto tmpl = fmt::format("{} ! {} ! {} ! {}", source, video_params, encoder->encoder_pipeline,
                          cfg.video.default_sink);

  long vbv = encoder_vbv_kbit(s.video.bitrate_kbps, s.video.fps);
  return fmt::format(
      fmt::runtime(tmpl), fmt::arg("node", render_node), fmt::arg("width", s.video.width),
      fmt::arg("height", s.video.height), fmt::arg("fps", s.video.fps),
      fmt::arg("color_range", color_range_str(s.video.color_range)),
      fmt::arg("color_space", color_space_str(s.video.color_space)),
      fmt::arg("bitrate", s.video.bitrate_kbps), fmt::arg("vbv_buffer_size", vbv),
      fmt::arg("slices_per_frame", s.video.slices_per_frame),
      fmt::arg("payload_size", s.video.packet_size),
      fmt::arg("fec_percentage", s.video.fec_percentage),
      fmt::arg("min_required_fec_packets", s.video.min_required_fec_packets));
}

std::string build_audio_pipeline(const session::StreamSession &s, bool use_test_src) {
  auto cfg = encoder_config::load_or_seed();
  const auto &a = cfg.audio;

  std::string src =
      use_test_src
          ? "audiotestsrc is-live=true wave=sine ! audiorate ! audioconvert ! audioresample"
          : fmt::format(fmt::runtime(a.default_source), fmt::arg("sink_name", a.sink_name),
                        fmt::arg("server",
                                 std::getenv("PULSE_SERVER") ? std::getenv("PULSE_SERVER") : ""));
  auto enc = fmt::format(fmt::runtime(a.default_opus_encoder),
                         fmt::arg("bitrate", a.bitrate_for_channels(s.audio.channels)),
                         fmt::arg("packet_duration", s.audio.packet_duration));
  // The caps (channel count + positioned channel-mask) are what keep opusenc in Opus channel
  // mapping family 1; without the mask surround falls into family 255, which Moonlight can't
  // decode against the advertised surround-params.
  return fmt::format(
      "{src} ! audio/x-raw,channels={ch},channel-mask=(bitmask){mask},rate=48000 ! {enc} ! "
      "rtpmoonlightpay_audio name=moonlight_pay packet_duration={pd} encrypt={aenc} "
      "aes_key=\"{key}\" aes_iv=\"{iv}\" ! "
      "appsink name=audio_sink sync=false",
      fmt::arg("src", src), fmt::arg("ch", s.audio.channels),
      fmt::arg("mask", channel_mask(s.audio.channels)), fmt::arg("enc", enc),
      fmt::arg("pd", s.audio.packet_duration),
      fmt::arg("aenc", s.audio.encrypt ? "true" : "false"), fmt::arg("key", s.aes_key),
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

  // Recreate the pulse sink with the negotiated channel count BEFORE the pipeline opens the
  // monitor source and before the app launches, so Steam/games detect 5.1/7.1 at startup.
  auto pulse_sink = encoder_config::load_or_seed().audio.sink_name;
  launcher::ensure_audio_sink(s->audio.channels, pulse_sink);
  ms->audio_channels_ = s->audio.channels;

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
  // Cold-plug controller 0 now, BEFORE the app (gamescope/Steam) launches, so the common single-
  // controller case is present for Steam/SDL's initial scan. Additional controllers are hotplugged
  // on demand (gamepad_arrival / gamepad_update) as the client connects them.
  {
    std::lock_guard<std::mutex> lk(ms->gamepad_mtx_);
    if (auto pad = input::VirtualGamepad::create())
      ms->gamepads_[0] = std::move(pad);
    else
      logs::log(logs::warning, "[MEDIA] failed to create virtual gamepad 0 at session start");
  }

  if (ms->video_pipeline_) {
    ms->wayland_src_ = gst_bin_get_by_name(GST_BIN(ms->video_pipeline_), "wolf_wayland_source");
    if (!ms->wayland_src_)
      logs::log(logs::warning, "[MEDIA] wolf_wayland_source not found -- input injection disabled");
    watch_wayland_and_launch(ms->video_pipeline_, s, ms->app_pid_, pulse_sink);
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

void MediaSession::gamepad_arrival(int controller_number) {
  std::lock_guard<std::mutex> lk(gamepad_mtx_);
  if (gamepads_.count(controller_number))
    return;
  if (auto pad = input::VirtualGamepad::create()) {
    gamepads_[controller_number] = std::move(pad);
    logs::log(logs::info, "[MEDIA] gamepad {} arrived (hotplug)", controller_number);
  } else {
    logs::log(logs::warning, "[MEDIA] failed to create gamepad {} on arrival", controller_number);
  }
}

void MediaSession::gamepad_update(int controller_number, std::uint16_t active_gamepad_mask,
                                  std::uint32_t button_flags, std::uint8_t left_trigger,
                                  std::uint8_t right_trigger, short left_x, short left_y,
                                  short right_x, short right_y) {
  std::lock_guard<std::mutex> lk(gamepad_mtx_);

  // Departure: Moonlight clears this controller's bit in the mask on disconnect. Tear the pad down
  // (its dtor sends the fake-udev "remove") so the running game sees the unplug.
  bool present = active_gamepad_mask & (1u << controller_number);
  if (!present) {
    if (gamepads_.erase(controller_number))
      logs::log(logs::info, "[MEDIA] gamepad {} removed (hotplug)", controller_number);
    return;
  }

  // Lazily create on first mention -- old Moonlight clients don't send CONTROLLER_ARRIVAL.
  auto it = gamepads_.find(controller_number);
  if (it == gamepads_.end()) {
    auto pad = input::VirtualGamepad::create();
    if (!pad) {
      logs::log(logs::warning, "[MEDIA] failed to create gamepad {} on first update",
                controller_number);
      return;
    }
    it = gamepads_.emplace(controller_number, std::move(pad)).first;
    logs::log(logs::info, "[MEDIA] gamepad {} created (lazy)", controller_number);
  }
  it->second->update(button_flags, left_trigger, right_trigger, left_x, left_y, right_x, right_y);
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

void MediaSession::rebuild_audio(const session::StreamSession &s) {
  if (audio_pipeline_) {
    gst_element_set_state(audio_pipeline_, GST_STATE_NULL);
    gst_object_unref(audio_pipeline_);
    audio_pipeline_ = nullptr;
  }
  bool audio_test = std::getenv("STEAM_STREAM_AUDIO_TEST") != nullptr;
  auto apipe = build_audio_pipeline(s, audio_test);
  logs::log(logs::info, "[MEDIA] restarting audio pipeline ({}ch):\n{}", s.audio.channels, apipe);
  audio_pipeline_ = launch(apipe, "audio_sink", audio_sink_);
  audio_channels_ = s.audio.channels;
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
  {
    // Destroy all virtual pads (each dtor sends the fake-udev "remove") so a resumed/next session
    // starts clean and the game sees the controllers unplug.
    std::lock_guard<std::mutex> lk(gamepad_mtx_);
    gamepads_.clear();
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
