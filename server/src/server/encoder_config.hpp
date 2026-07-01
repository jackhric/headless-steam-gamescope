#pragma once

// File-backed, round-trippable encoder configuration. The live config is a TOML file on the
// HOST (bind-mounted to /config/encoders.toml; written from a compiled-in seed if absent) so
// a future web UI can read/edit it; the server reloads it fresh at every stream start.
// See config/encoders.toml.

#include <optional>
#include <server/session.hpp>
#include <string>
#include <vector>

namespace encoder_config {

// One candidate encoder for a codec. Wolf-style: try each in order, pick the first whose
// GStreamer elements are all instantiable on this box.
struct GstEncoder {
  std::string plugin_name;
  std::vector<std::string> check_elements;
  std::string encoder_pipeline;
  std::string video_params;                        // RGBx/system-memory capture path
  std::optional<std::string> video_params_zero_copy; // GPU-memory path (WOLF_USE_ZERO_COPY)
};

struct GstVideoCfg {
  std::string default_source;
  // GPU-buffer capture head (waylanddisplaysrc outputs memory:CUDAMemory directly, no
  // cudaupload). Used with an encoder's video_params_zero_copy when WOLF_USE_ZERO_COPY is on
  // and the selected encoder supports it. Absent -> zero-copy unavailable, use default_source.
  std::optional<std::string> default_source_zero_copy;
  std::string default_sink;
  std::vector<GstEncoder> h264_encoders;
  std::vector<GstEncoder> hevc_encoders;
  std::vector<GstEncoder> av1_encoders;
};

struct EncoderConfig {
  GstVideoCfg video;
};

// Per-codec availability, for a future web UI to render valid choices.
struct Availability {
  bool h264 = false;
  bool hevc = false;
  bool av1 = false;
};

// The compiled-in seed (Wolf encoder blocks + steam-stream source/sink). Also what
// create_default_if_missing writes to disk.
EncoderConfig defaults();

// Resolve the on-disk config path: $STEAM_STREAM_ENCODER_CONFIG (default /config/encoders.toml,
// a host bind mount), else $STEAM_STREAM_STATE_DIR/encoders.toml as a last-resort fallback.
std::string config_path();

// Web-UI seam functions (no HTTP yet; these are what endpoints will call later).
std::optional<EncoderConfig> load(const std::string &path);
bool save(const EncoderConfig &cfg, const std::string &path);
void create_default_if_missing(const std::string &path);
std::vector<std::string> validate(const EncoderConfig &cfg);
Availability available_encoders(const EncoderConfig &cfg);

// Load from config_path(), writing the seed first if absent; on any error fall back to
// the compiled-in defaults so the server always has a usable config.
EncoderConfig load_or_seed();

// True if every check_elements entry can be instantiated on this box.
bool is_available(const GstEncoder &enc);

// First available encoder for the requested codec; falls back to H264 if the requested
// codec has none available. Returns nullopt only if H264 is also unavailable.
std::optional<GstEncoder> select_encoder(const EncoderConfig &cfg, session::VideoFormat fmt);

} // namespace encoder_config
