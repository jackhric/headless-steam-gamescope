#pragma once

#include <crypto/crypto.hpp>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <server/moonlight_proto.hpp>
#include <server/session.hpp>
#include <string>
#include <vector>

namespace state {

enum class PAIR_PHASE { NONE, GETSERVERCERT, CLIENTCHALLENGE, SERVERCHALLENGERESP, CLIENTPAIRINGSECRET };

// In-flight state for a single client's multi-step PIN handshake.
struct PairCache {
  std::string client_cert; // PEM
  std::string aes_key;
  PAIR_PHASE last_phase = PAIR_PHASE::NONE;

  std::optional<std::string> server_secret;
  std::optional<std::string> server_challenge;
  std::optional<std::string> client_hash;
};

struct PairedClient {
  std::string client_cert; // PEM
};

// Process-wide server state. Thread-safe: SimpleWeb dispatches handlers from a pool of
// asio threads.
class AppState {
public:
  x509::x509_ptr server_cert;
  x509::pkey_ptr server_pkey;
  std::string cert_path;
  std::string key_path;

  std::string uuid;
  std::string hostname;
  std::string mac_address;

  int http_port = 47989;
  int https_port = 47984;
  int rtsp_port = 48010;
  // RTP/control ports advertised over RTSP SETUP.
  unsigned short video_stream_port = 48100;
  unsigned short audio_stream_port = 48200;
  unsigned short control_stream_port = 47999;

  bool support_hevc = false;
  bool support_av1 = false;

  std::vector<moonlight::DisplayMode> display_modes;
  std::vector<moonlight::App> apps;

  // shared_ptr keeps AppState movable (SessionRegistry holds a non-movable mutex).
  std::shared_ptr<session::SessionRegistry> sessions = std::make_shared<session::SessionRegistry>();

  // Installed by main.cpp; lets /cancel tear down the live session (registry removal alone
  // leaves the app + pipelines orphaned).
  std::function<void(std::size_t session_id)> stop_session;

  // Load (or generate + persist) server cert/key, uuid, paired clients from state_dir.
  static AppState init(const std::string &state_dir);

  std::optional<PairCache> get_pair_cache(const std::string &key);
  void set_pair_cache(const std::string &key, const PairCache &v);
  void remove_pair_cache(const std::string &key);

  void add_paired_client(const std::string &client_cert_pem);
  void remove_paired_client(const std::string &client_cert_pem);
  // Validates the presented cert against the store.
  std::optional<PairedClient> get_client_via_ssl(const x509::x509_ptr &client_cert);
  std::vector<std::string> paired_certs();

  std::string state_dir;

private:
  // unique_ptr so AppState stays movable (init() returns by value); std::mutex is not.
  std::unique_ptr<std::mutex> mtx_ = std::make_unique<std::mutex>();
  std::map<std::string, PairCache> pairing_cache_;
  std::vector<std::string> paired_clients_; // PEM
};

} // namespace state
