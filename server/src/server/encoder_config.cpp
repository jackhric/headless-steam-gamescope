#include <server/encoder_config.hpp>

#include <cstdlib>
#include <fmt/format.h>
#include <fstream>
#include <gst/gst.h>
#include <helpers/logger.hpp>
#include <sstream>
#include <toml++/toml.hpp>

namespace encoder_config {

namespace {

std::string env_or(const char *k, const std::string &def) {
  const char *v = std::getenv(k);
  return v ? std::string(v) : def;
}

// steam-stream's own capture head + payloader/appsink. Unlike Wolf (interpipesrc/queue),
// we capture directly from the virtual compositor and hand buffers to our UDPSink appsink.
// The encoder element MUST be named video_encoder so MediaSession::update_bitrate can find it.
constexpr const char *kDefaultSource =
    "waylanddisplaysrc name=wolf_wayland_source render-node={node} ! "
    "video/x-raw,format=RGBx,width={width},height={height},framerate={fps}/1";

// Zero-copy capture head: cuda-device-id makes waylanddisplaysrc create its own CUDA context
// and output memory:CUDAMemory buffers directly (BGRA/RGBA), so no system-RAM download and no
// cudaupload. Paired with kNvVideoParamsZeroCopy. Verified working under CDI on this box.
constexpr const char *kDefaultSourceZeroCopy =
    "waylanddisplaysrc name=wolf_wayland_source render-node={node} cuda-device-id=0 ! "
    "video/x-raw(memory:CUDAMemory),width={width},height={height},framerate={fps}/1";

constexpr const char *kDefaultSink =
    "rtpmoonlightpay_video name=moonlight_pay payload_size={payload_size} "
    "fec_percentage={fec_percentage} min_required_fec_packets={min_required_fec_packets} ! "
    "appsink name=video_sink sync=false max-buffers=1 drop=true";

constexpr const char *kDefaultAudioSinkName = "steam-stream";
constexpr const char *kDefaultAudioSource =
    "pulsesrc device=\"{sink_name}.monitor\" server=\"{server}\" ! "
    "audiorate ! audioconvert ! audioresample";
// CBR + restricted-lowdelay + fullband are what Moonlight's decoder expects; the caps upstream
// (channels + channel-mask) keep opusenc in channel-mapping-family 1 for surround.
constexpr const char *kDefaultOpusEncoder =
    "opusenc bitrate={bitrate} bitrate-type=cbr frame-size={packet_duration} bandwidth=fullband "
    "audio-type=restricted-lowdelay max-payload-size=1400";

// Non-zero-copy path: system RGBx -> cudaupload -> GPU NV12. The zero-copy variant drops
// cudaupload because the buffer is already CUDA-resident (see kDefaultSourceZeroCopy) -- only
// valid when paired with that source.
constexpr const char *kNvVideoParams =
    "cudaupload ! cudaconvertscale add-borders=true ! "
    "video/x-raw(memory:CUDAMemory),width={width},height={height},chroma-site={color_range},"
    "format=NV12,colorimetry={color_space},pixel-aspect-ratio=1/1";
constexpr const char *kNvVideoParamsZeroCopy =
    "cudaconvertscale add-borders=true ! "
    "video/x-raw(memory:CUDAMemory),width={width},height={height},chroma-site={color_range},"
    "format=NV12,colorimetry={color_space},pixel-aspect-ratio=1/1";

constexpr const char *kVaVideoParams =
    "vapostproc ! video/x-raw(memory:VAMemory),format=NV12,width={width},height={height},"
    "pixel-aspect-ratio=1/1";

constexpr const char *kSwVideoParams =
    "videoconvertscale ! videorate ! "
    "video/x-raw,width={width},height={height},framerate={fps}/1,format=I420,"
    "chroma-site={color_range},colorimetry={color_space}";

// repeat-sequence-header / config-interval=-1: every IDR must carry VPS/SPS/PPS -- a resumed
// client starts a fresh decoder mid-stream and can only sync on a self-contained keyframe.
GstEncoder nv_h264() {
  return {"nvcodec",
          {"nvh264enc", "cudaconvertscale", "cudaupload"},
          "nvh264enc name=video_encoder preset=low-latency-hq zerolatency=true gop-size=0 "
          "rc-mode=cbr-ld-hq bitrate={bitrate} vbv-buffer-size={vbv_buffer_size} aud=false "
          "repeat-sequence-header=true ! "
          "h264parse config-interval=-1 ! video/x-h264,profile=main,stream-format=byte-stream",
          kNvVideoParams,
          kNvVideoParamsZeroCopy};
}

GstEncoder va_h264() {
  return {"va",
          {"vah264enc", "vapostproc"},
          "vah264enc name=video_encoder aud=false b-frames=0 ref-frames=1 "
          "num-slices={slices_per_frame} bitrate={bitrate} cpb-size={vbv_buffer_size} min-qp=20 "
          "key-int-max=1024 rate-control=cbr target-usage=6 ! "
          "h264parse config-interval=-1 ! video/x-h264,profile=main,stream-format=byte-stream",
          kVaVideoParams,
          std::nullopt};
}

GstEncoder sw_h264() {
  return {"x264",
          {"x264enc"},
          "x264enc name=video_encoder pass=qual tune=zerolatency speed-preset=superfast "
          "b-adapt=false bframes=0 ref=1 sliced-threads=true threads={slices_per_frame} "
          "option-string=\"slices={slices_per_frame}:keyint=infinite:open-gop=0\" "
          "bitrate={bitrate} aud=false ! "
          "h264parse config-interval=-1 ! video/x-h264,profile=high,stream-format=byte-stream",
          kSwVideoParams,
          std::nullopt};
}

GstEncoder nv_hevc() {
  return {"nvcodec",
          {"nvh265enc", "cudaconvertscale", "cudaupload"},
          "nvh265enc name=video_encoder gop-size=-1 bitrate={bitrate} "
          "vbv-buffer-size={vbv_buffer_size} aud=false rc-mode=cbr zerolatency=true preset=p1 "
          "tune=ultra-low-latency multi-pass=two-pass-quarter repeat-sequence-header=true ! "
          "h265parse config-interval=-1 ! video/x-h265,profile=main,stream-format=byte-stream",
          kNvVideoParams,
          kNvVideoParamsZeroCopy};
}

GstEncoder va_hevc() {
  return {"va",
          {"vah265enc", "vapostproc"},
          "vah265enc name=video_encoder aud=false b-frames=0 ref-frames=1 "
          "num-slices={slices_per_frame} bitrate={bitrate} cpb-size={vbv_buffer_size} min-qp=20 "
          "key-int-max=1024 rate-control=cbr target-usage=6 ! "
          "h265parse config-interval=-1 ! video/x-h265,profile=main,stream-format=byte-stream",
          kVaVideoParams,
          std::nullopt};
}

GstEncoder nv_av1() {
  return {"nvcodec",
          {"nvav1enc", "cudaconvertscale", "cudaupload"},
          "nvav1enc name=video_encoder gop-size=-1 bitrate={bitrate} "
          "vbv-buffer-size={vbv_buffer_size} rc-mode=cbr zerolatency=true preset=p1 "
          "tune=ultra-low-latency multi-pass=two-pass-quarter ! "
          "av1parse ! video/x-av1,stream-format=obu-stream,alignment=frame,profile=main",
          kNvVideoParams,
          kNvVideoParamsZeroCopy};
}

// --- toml++ (de)serialization -------------------------------------------------

toml::table encoder_to_toml(const GstEncoder &e) {
  toml::array checks;
  for (const auto &c : e.check_elements)
    checks.push_back(c);
  toml::table t;
  t.insert("plugin_name", e.plugin_name);
  t.insert("check_elements", std::move(checks));
  t.insert("encoder_pipeline", e.encoder_pipeline);
  t.insert("video_params", e.video_params);
  if (e.video_params_zero_copy)
    t.insert("video_params_zero_copy", *e.video_params_zero_copy);
  return t;
}

GstEncoder encoder_from_toml(const toml::table &t) {
  GstEncoder e;
  e.plugin_name = t["plugin_name"].value_or("");
  if (auto arr = t["check_elements"].as_array())
    for (const auto &el : *arr)
      if (auto s = el.value<std::string>())
        e.check_elements.push_back(*s);
  e.encoder_pipeline = t["encoder_pipeline"].value_or("");
  e.video_params = t["video_params"].value_or("");
  if (auto z = t["video_params_zero_copy"].value<std::string>())
    e.video_params_zero_copy = *z;
  return e;
}

toml::array encoders_to_toml(const std::vector<GstEncoder> &encs) {
  toml::array arr;
  for (const auto &e : encs)
    arr.push_back(encoder_to_toml(e));
  return arr;
}

std::vector<GstEncoder> encoders_from_toml(const toml::array *arr) {
  std::vector<GstEncoder> out;
  if (!arr)
    return out;
  for (const auto &node : *arr)
    if (auto t = node.as_table())
      out.push_back(encoder_from_toml(*t));
  return out;
}

} // namespace

EncoderConfig defaults() {
  EncoderConfig cfg;
  cfg.video.default_source = kDefaultSource;
  cfg.video.default_source_zero_copy = kDefaultSourceZeroCopy;
  cfg.video.default_sink = kDefaultSink;
  cfg.video.h264_encoders = {nv_h264(), va_h264(), sw_h264()};
  cfg.video.hevc_encoders = {nv_hevc(), va_hevc()};
  cfg.video.av1_encoders = {nv_av1()};
  cfg.audio.sink_name = kDefaultAudioSinkName;
  cfg.audio.default_source = kDefaultAudioSource;
  cfg.audio.default_opus_encoder = kDefaultOpusEncoder;
  return cfg;
}

std::string config_path() {
  if (const char *p = std::getenv("STEAM_STREAM_ENCODER_CONFIG"))
    return p;
  auto state_dir = env_or("STEAM_STREAM_STATE_DIR", "/var/lib/steam-stream");
  return state_dir + "/encoders.toml";
}

std::optional<EncoderConfig> load(const std::string &path) {
  toml::table root;
  try {
    root = toml::parse_file(path);
  } catch (const toml::parse_error &err) {
    logs::log(logs::error, "[ENCODER-CFG] parse error in {}: {}", path,
              std::string(err.description()));
    return std::nullopt;
  }

  auto video = root["video"].as_table();
  if (!video) {
    logs::log(logs::error, "[ENCODER-CFG] {} missing [video] table", path);
    return std::nullopt;
  }

  EncoderConfig cfg;
  cfg.video.default_source = (*video)["default_source"].value_or("");
  if (auto z = (*video)["default_source_zero_copy"].value<std::string>())
    cfg.video.default_source_zero_copy = *z;
  cfg.video.default_sink = (*video)["default_sink"].value_or("");
  cfg.video.h264_encoders = encoders_from_toml((*video)["h264_encoders"].as_array());
  cfg.video.hevc_encoders = encoders_from_toml((*video)["hevc_encoders"].as_array());
  cfg.video.av1_encoders = encoders_from_toml((*video)["av1_encoders"].as_array());

  // [audio] is optional so configs seeded before it existed keep working on the defaults.
  const auto d = defaults().audio;
  cfg.audio = d;
  if (auto audio = root["audio"].as_table()) {
    cfg.audio.sink_name = (*audio)["sink_name"].value_or(d.sink_name);
    cfg.audio.default_source = (*audio)["default_source"].value_or(d.default_source);
    cfg.audio.default_opus_encoder = (*audio)["default_opus_encoder"].value_or(d.default_opus_encoder);
    cfg.audio.bitrate_stereo = (*audio)["bitrate_stereo"].value_or(d.bitrate_stereo);
    cfg.audio.bitrate_51 = (*audio)["bitrate_51"].value_or(d.bitrate_51);
    cfg.audio.bitrate_71 = (*audio)["bitrate_71"].value_or(d.bitrate_71);
  }
  return cfg;
}

bool save(const EncoderConfig &cfg, const std::string &path) {
  toml::table video;
  video.insert("default_source", cfg.video.default_source);
  if (cfg.video.default_source_zero_copy)
    video.insert("default_source_zero_copy", *cfg.video.default_source_zero_copy);
  video.insert("default_sink", cfg.video.default_sink);
  video.insert("h264_encoders", encoders_to_toml(cfg.video.h264_encoders));
  video.insert("hevc_encoders", encoders_to_toml(cfg.video.hevc_encoders));
  video.insert("av1_encoders", encoders_to_toml(cfg.video.av1_encoders));

  toml::table audio;
  audio.insert("sink_name", cfg.audio.sink_name);
  audio.insert("default_source", cfg.audio.default_source);
  audio.insert("default_opus_encoder", cfg.audio.default_opus_encoder);
  audio.insert("bitrate_stereo", cfg.audio.bitrate_stereo);
  audio.insert("bitrate_51", cfg.audio.bitrate_51);
  audio.insert("bitrate_71", cfg.audio.bitrate_71);

  toml::table root;
  root.insert("video", std::move(video));
  root.insert("audio", std::move(audio));

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    logs::log(logs::error, "[ENCODER-CFG] cannot write {}", path);
    return false;
  }
  out << root;
  return static_cast<bool>(out);
}

void create_default_if_missing(const std::string &path) {
  std::ifstream probe(path);
  if (probe.good())
    return;
  logs::log(logs::info, "[ENCODER-CFG] no config at {}, writing defaults", path);
  if (!save(defaults(), path))
    logs::log(logs::warning, "[ENCODER-CFG] failed to write default config to {}", path);
}

std::vector<std::string> validate(const EncoderConfig &cfg) {
  std::vector<std::string> issues;
  if (cfg.video.default_source.empty())
    issues.push_back("video.default_source is empty");
  if (cfg.video.default_sink.empty())
    issues.push_back("video.default_sink is empty");
  if (cfg.video.h264_encoders.empty())
    issues.push_back("video.h264_encoders is empty (H264 is the required fallback codec)");

  auto check_list = [&](const char *name, const std::vector<GstEncoder> &encs) {
    for (size_t i = 0; i < encs.size(); ++i) {
      if (encs[i].plugin_name.empty())
        issues.push_back(fmt::format("{}[{}] has empty plugin_name", name, i));
      if (encs[i].encoder_pipeline.empty())
        issues.push_back(fmt::format("{}[{}] has empty encoder_pipeline", name, i));
      if (encs[i].encoder_pipeline.find("name=video_encoder") == std::string::npos)
        issues.push_back(fmt::format(
            "{}[{}] encoder_pipeline lacks 'name=video_encoder' (update_bitrate needs it)", name,
            i));
    }
  };
  check_list("h264_encoders", cfg.video.h264_encoders);
  check_list("hevc_encoders", cfg.video.hevc_encoders);
  check_list("av1_encoders", cfg.video.av1_encoders);

  if (cfg.audio.sink_name.empty())
    issues.push_back("audio.sink_name is empty");
  if (cfg.audio.default_source.empty())
    issues.push_back("audio.default_source is empty");
  if (cfg.audio.default_opus_encoder.empty())
    issues.push_back("audio.default_opus_encoder is empty");
  return issues;
}

bool is_available(const GstEncoder &enc) {
  for (const auto &el_name : enc.check_elements) {
    GstElement *el = gst_element_factory_make(el_name.c_str(), nullptr);
    if (!el) {
      logs::log(logs::debug, "[ENCODER-CFG] element unavailable: {}", el_name);
      return false;
    }
    gst_object_unref(el);
  }
  return true;
}

namespace {
std::optional<GstEncoder> first_available(const std::vector<GstEncoder> &encs) {
  for (const auto &e : encs)
    if (is_available(e))
      return e;
  return std::nullopt;
}
} // namespace

Availability available_encoders(const EncoderConfig &cfg) {
  return {first_available(cfg.video.h264_encoders).has_value(),
          first_available(cfg.video.hevc_encoders).has_value(),
          first_available(cfg.video.av1_encoders).has_value()};
}

std::optional<GstEncoder> select_encoder(const EncoderConfig &cfg, session::VideoFormat fmt) {
  const char *codec = "H264";
  const std::vector<GstEncoder> *list = &cfg.video.h264_encoders;
  switch (fmt) {
  case session::VideoFormat::HEVC:
    codec = "HEVC";
    list = &cfg.video.hevc_encoders;
    break;
  case session::VideoFormat::AV1:
    codec = "AV1";
    list = &cfg.video.av1_encoders;
    break;
  case session::VideoFormat::H264:
    break;
  }

  if (auto e = first_available(*list)) {
    logs::log(logs::info, "[ENCODER-CFG] using {} encoder: {}", codec, e->plugin_name);
    return e;
  }

  if (fmt != session::VideoFormat::H264) {
    logs::log(logs::warning, "[ENCODER-CFG] no available {} encoder, falling back to H264", codec);
    if (auto e = first_available(cfg.video.h264_encoders)) {
      logs::log(logs::info, "[ENCODER-CFG] using H264 encoder: {}", e->plugin_name);
      return e;
    }
  }
  logs::log(logs::error, "[ENCODER-CFG] no available encoder for {} (and no H264 fallback)", codec);
  return std::nullopt;
}

EncoderConfig load_or_seed() {
  auto path = config_path();
  create_default_if_missing(path);
  if (auto cfg = load(path)) {
    auto issues = validate(*cfg);
    for (const auto &i : issues)
      logs::log(logs::warning, "[ENCODER-CFG] {}: {}", path, i);
    return *cfg;
  }
  logs::log(logs::warning, "[ENCODER-CFG] falling back to compiled-in defaults");
  return defaults();
}

} // namespace encoder_config
