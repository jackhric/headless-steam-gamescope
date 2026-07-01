#pragma once

// aes_key/aes_iv are stored VERBATIM (the rikey/rikeyid launch params) and passed unchanged
// into the payloader element's aes-key/aes-iv properties -- do NOT decode or transform them.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace session {

enum class VideoFormat { H264, HEVC, AV1 };

// Moonlight encoderCscMode bit layout: bit0 = range, bits>>1 = colour space.
enum class ColorRange { MPEG, JPEG };
enum class ColorSpace { BT601, BT709, BT2020 };

struct VideoParams {
  int width = 0;
  int height = 0;
  int fps = 0;
  VideoFormat format = VideoFormat::H264;

  int packet_size = 1392;
  int fec_percentage = 20;
  int min_required_fec_packets = 2;
  long bitrate_kbps = 15500;
  int slices_per_frame = 1;
  int frames_with_invalid_ref_threshold = 0;
  int timeout_ms = 7000;

  ColorRange color_range = ColorRange::MPEG;
  ColorSpace color_space = ColorSpace::BT601;
};

struct AudioParams {
  bool encrypt = false;
  int channels = 2;
  int streams = 1;
  int coupled_streams = 1;
  int bitrate = 96000;
  int packet_duration = 5;
};

struct StreamSession {
  std::size_t session_id = 0;
  std::string client_ip;
  std::string rtsp_fake_ip; // handed back in sessionUrl0; Moonlight parrots it back in the
                            // RTSP URI/Host so we can re-find this session.

  std::string app_id; // applist ID ("1" = Steam Big Picture, "2" = Steam Desktop)
  std::string app_name;

  std::string aes_key; // rikey, hex string
  std::string aes_iv;  // rikeyid, decimal string

  int client_width = 0;
  int client_height = 0;
  int client_fps = 0;
  int audio_channel_count = 2;

  bool hevc_supported = false;
  bool av1_supported = false;

  unsigned short video_stream_port = 0;
  unsigned short audio_stream_port = 0;
  unsigned short control_stream_port = 0;

  // Moonlight IP-less extension payloads (returned in SETUP).
  std::array<char, 16> rtp_secret_payload{};
  std::uint32_t enet_secret_payload = 0;

  bool announced = false;
  bool playing = false;
  VideoParams video;
  AudioParams audio;

  // Guards the ANNOUNCE/PLAY-mutated fields (RTSP runs on its own thread).
  std::shared_ptr<std::mutex> mtx = std::make_shared<std::mutex>();
};

class SessionRegistry {
public:
  void add(const std::shared_ptr<StreamSession> &s) {
    std::lock_guard<std::mutex> lk(m_);
    sessions_.push_back(s);
  }

  void remove(std::size_t session_id) {
    std::lock_guard<std::mutex> lk(m_);
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [&](const auto &s) { return s->session_id == session_id; }),
                    sessions_.end());
  }

  std::shared_ptr<StreamSession> get_by_client_ip(const std::string &ip) {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto &s : sessions_)
      if (s->client_ip == ip)
        return s;
    return nullptr;
  }

  // Match the parroted fake IP first (URI ip or Host header), then fall back to the real
  // peer IP when the client sent 0.0.0.0 / no Host.
  std::shared_ptr<StreamSession>
  get_for_rtsp(const std::string &uri_ip, const std::string &host_opt, const std::string &peer_ip) {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto &s : sessions_) {
      if (s->rtsp_fake_ip == uri_ip || host_opt == s->rtsp_fake_ip)
        return s;
      if ((host_opt == "0.0.0.0" || host_opt.empty()) && s->client_ip == peer_ip)
        return s;
    }
    return nullptr;
  }

  std::shared_ptr<StreamSession> get_running() {
    std::lock_guard<std::mutex> lk(m_);
    return sessions_.empty() ? nullptr : sessions_.front();
  }

  std::shared_ptr<StreamSession> get_by_id(std::size_t id) {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto &s : sessions_)
      if (s->session_id == id)
        return s;
    return nullptr;
  }

  bool empty() {
    std::lock_guard<std::mutex> lk(m_);
    return sessions_.empty();
  }

  std::size_t next_id() {
    return next_id_.fetch_add(1);
  }

private:
  std::mutex m_;
  std::vector<std::shared_ptr<StreamSession>> sessions_;
  std::atomic<std::size_t> next_id_{1};
};

} // namespace session
