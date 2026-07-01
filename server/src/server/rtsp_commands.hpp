#pragma once

// Moonlight RTSP verb handlers (OPTIONS / DESCRIBE / SETUP / ANNOUNCE / PLAY) as pure
// functions over a StreamSession.

#include <algorithm>
#include <functional>
#include <helpers/logger.hpp>
#include <map>
#include <optional>
#include <rtsp/parser.hpp>
#include <server/session.hpp>
#include <string>
#include <vector>

namespace rtsp {

namespace audio_cfg {
enum Speaker { FRONT_LEFT, FRONT_RIGHT, FRONT_CENTER, LOW_FREQUENCY, BACK_LEFT, BACK_RIGHT, SIDE_LEFT, SIDE_RIGHT };

struct Config {
  int channels;
  int streams;
  int coupled_streams;
  int bitrate;
  std::vector<Speaker> speakers;
};

inline const std::vector<Config> &configurations() {
  static const std::vector<Config> cfgs = {
      {2, 1, 1, 96000, {FRONT_LEFT, FRONT_RIGHT}},
      {6, 4, 2, 256000, {FRONT_LEFT, FRONT_RIGHT, FRONT_CENTER, LOW_FREQUENCY, BACK_LEFT, BACK_RIGHT}},
      {8, 5, 3, 450000,
       {FRONT_LEFT, FRONT_RIGHT, FRONT_CENTER, LOW_FREQUENCY, BACK_LEFT, BACK_RIGHT, SIDE_LEFT, SIDE_RIGHT}}};
  return cfgs;
}

// GFE channel remap + rotate: Moonlight needs this exact ordering or it can't decode
// surround audio.
inline std::string speaker_string(const Config &c) {
  std::vector<Speaker> m = c.speakers;
  if (c.channels == 6) {
    m = {FRONT_LEFT, FRONT_RIGHT, BACK_LEFT, BACK_RIGHT, FRONT_CENTER, LOW_FREQUENCY};
  } else if (c.channels == 8) {
    m = {FRONT_LEFT, FRONT_RIGHT, BACK_LEFT, BACK_RIGHT, FRONT_CENTER, LOW_FREQUENCY, SIDE_LEFT, SIDE_RIGHT};
  }
  if (c.channels > 2) {
    std::rotate(m.begin() + 3, m.begin() + 4, m.end());
  }
  std::string s;
  for (auto sp : m)
    s += static_cast<char>(static_cast<int>(sp) + '0');
  return s;
}

inline const Config &for_channels(int channels) {
  for (const auto &c : configurations())
    if (c.channels == channels)
      return c;
  return configurations().front();
}
} // namespace audio_cfg

constexpr std::uint32_t FS_PEN_TOUCH_EVENTS = 0x01;
constexpr std::uint32_t FS_CONTROLLER_TOUCH_EVENTS = 0x02;

inline RTSP_PACKET error_msg(unsigned short status_code, std::string_view msg, int seq = 0) {
  return {.type = RESPONSE, .seq_number = seq, .response = {.status_code = status_code, .msg = std::string(msg)}};
}

inline RTSP_PACKET ok_msg(int seq,
                          const std::map<std::string, std::string> &options = {},
                          const std::vector<std::pair<std::string, std::string>> &payloads = {}) {
  return {.type = RESPONSE,
          .seq_number = seq,
          .response = {.status_code = 200, .msg = "OK"},
          .options = options,
          .payloads = payloads};
}

inline RTSP_PACKET describe(const RTSP_PACKET &req, const session::StreamSession &s) {
  std::vector<std::pair<std::string, std::string>> payloads;
  if (s.hevc_supported)
    payloads.push_back({"", "sprop-parameter-sets=AAAAAU"});
  if (s.av1_supported)
    payloads.push_back({"a", "a=rtpmap:98 AV1/90000"});

  for (const auto &cfg : audio_cfg::configurations()) {
    auto sp = audio_cfg::speaker_string(cfg);
    auto surround = fmt::format("fmtp:97 surround-params={}{}{}{}", cfg.channels, cfg.streams, cfg.coupled_streams, sp);
    payloads.push_back({"a", surround});
  }

  payloads.push_back({"a", fmt::format("x-ss-general.featureFlags: {}", FS_PEN_TOUCH_EVENTS | FS_CONTROLLER_TOUCH_EVENTS)});
  return ok_msg(req.seq_number, {}, payloads);
}

inline RTSP_PACKET setup(const RTSP_PACKET &req, const session::StreamSession &s) {
  const auto &type = req.request.stream.type;
  std::map<std::string, std::string> options = {{"Session", "DEADBEEFCAFE;timeout = 90"}};

  if (type == "audio") {
    options["Transport"] = "server_port=" + std::to_string(s.audio_stream_port);
    options["X-SS-Ping-Payload"] = {s.rtp_secret_payload.begin(), s.rtp_secret_payload.end()};
  } else if (type == "video") {
    options["Transport"] = "server_port=" + std::to_string(s.video_stream_port);
    options["X-SS-Ping-Payload"] = {s.rtp_secret_payload.begin(), s.rtp_secret_payload.end()};
  } else if (type == "control") {
    options["Transport"] = "server_port=" + std::to_string(s.control_stream_port);
    options["X-SS-Connect-Data"] = std::to_string(s.enet_secret_payload);
  } else {
    return error_msg(404, "NOT FOUND", req.seq_number);
  }
  return ok_msg(req.seq_number, options);
}

// "x-nv-video[0].clientViewportWd:1920" -> {"x-nv-video[0].clientViewportWd", 1920}
inline std::pair<std::string, std::optional<int>> parse_arg_line(const std::pair<std::string, std::string> &line) {
  auto colon = line.second.find(':');
  if (colon == std::string::npos)
    return {line.second, std::nullopt};
  auto key = line.second.substr(0, colon);
  std::optional<int> val;
  try {
    val = std::stoi(line.second.substr(colon + 1));
  } catch (const std::exception &) {
    val = std::nullopt;
  }
  return {key, val};
}

// Captures the client's negotiated video/audio params from its SDP into the session.
inline RTSP_PACKET announce(const RTSP_PACKET &req, session::StreamSession &s) {
  std::map<std::string, std::optional<int>> args;
  for (const auto &line : req.payloads) {
    if (line.first == "a") {
      auto [k, v] = parse_arg_line(line);
      args[k] = v;
    }
  }
  auto arg = [&](const std::string &k, int def) -> int {
    auto it = args.find(k);
    return (it != args.end() && it->second) ? *it->second : def;
  };

  int bit_stream_format = arg("x-nv-vqos[0].bitStreamFormat", 0);
  int csc = arg("x-nv-video[0].encoderCscMode", 0);
  int audio_channels = arg("x-nv-audio.surround.numChannels", s.audio_channel_count);
  int fec_percentage = 20;

  long bitrate = arg("x-nv-vqos[0].bw.maximumBitrateKbps", 15500);
  if (auto it = args.find("x-ml-video.configuredBitrateKbps"); it != args.end() && it->second) {
    bitrate = *it->second;
    if (fec_percentage <= 80)
      bitrate /= 100.f / (100 - fec_percentage);
    auto audio_adj = 96 * audio_channels;
    bitrate -= std::min(static_cast<long>(audio_adj), bitrate / 5);
    bitrate -= std::min(500L, bitrate / 10);
  }

  std::lock_guard<std::mutex> lk(*s.mtx);
  auto &v = s.video;
  v.width = arg("x-nv-video[0].clientViewportWd", s.client_width);
  v.height = arg("x-nv-video[0].clientViewportHt", s.client_height);
  v.fps = arg("x-nv-video[0].maxFPS", s.client_fps);
  v.format = bit_stream_format == 2 ? session::VideoFormat::AV1
             : bit_stream_format == 1 ? session::VideoFormat::HEVC
                                      : session::VideoFormat::H264;
  v.packet_size = arg("x-nv-video[0].packetSize", 1392);
  v.frames_with_invalid_ref_threshold = arg("x-nv-video[0].framesWithInvalidRefThreshold", 0);
  v.fec_percentage = fec_percentage;
  v.min_required_fec_packets = arg("x-nv-vqos[0].fec.minRequiredFecPackets", 2);
  v.bitrate_kbps = bitrate;
  v.slices_per_frame = arg("x-nv-video[0].videoEncoderSlicesPerFrame", 1);
  v.timeout_ms = arg("x-nv-video[0].timeoutLengthMs", 7000);
  v.color_range = (csc & 0x1) ? session::ColorRange::JPEG : session::ColorRange::MPEG;
  v.color_space = static_cast<session::ColorSpace>(csc >> 1);

  const auto &acfg = audio_cfg::for_channels(audio_channels);
  auto &a = s.audio;
  a.encrypt = static_cast<bool>(arg("x-nv-general.featureFlags", 167) & 0x20);
  a.channels = acfg.channels;
  a.streams = acfg.streams;
  a.coupled_streams = acfg.coupled_streams;
  a.bitrate = acfg.bitrate;
  a.packet_duration = arg("x-nv-aqos.packetDuration", 5);
  s.announced = true;

  logs::log(logs::info,
            "[RTSP] ANNOUNCE captured: {}x{}@{} fmt={} bitrate={}kbps fec={}% pkt={} audio={}ch encrypt={}",
            v.width, v.height, v.fps, static_cast<int>(v.format), v.bitrate_kbps, v.fec_percentage, v.packet_size,
            a.channels, a.encrypt);
  return ok_msg(req.seq_number);
}

inline RTSP_PACKET message_handler(const RTSP_PACKET &req,
                                   session::StreamSession &s,
                                   const std::function<void(session::StreamSession &)> &on_play = {}) {
  const auto &cmd = req.request.cmd;
  logs::log(logs::debug, "[RTSP] received command {}", cmd);
  if (cmd == "OPTIONS")
    return ok_msg(req.seq_number);
  if (cmd == "DESCRIBE")
    return describe(req, s);
  if (cmd == "SETUP")
    return setup(req, s);
  if (cmd == "ANNOUNCE")
    return announce(req, s);
  if (cmd == "PLAY") {
    {
      std::lock_guard<std::mutex> lk(*s.mtx);
      s.playing = true;
    }
    if (on_play)
      on_play(s);
    return ok_msg(req.seq_number);
  }
  logs::log(logs::warning, "[RTSP] command {} not found", cmd);
  return error_msg(404, "NOT FOUND", req.seq_number);
}

} // namespace rtsp
