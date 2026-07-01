#include <algorithm>
#include <filesystem>
#include <fstream>
#include <helpers/logger.hpp>
#include <netinet/in.h>
#include <random>
#include <server/state.hpp>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>

namespace fs = std::filesystem;

namespace state {

static std::string read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static void write_file(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << data;
}

static std::string gen_uuid() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 15);
  const char *hex = "0123456789abcdef";
  std::string out;
  const char *fmt = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
  for (char c : std::string(fmt)) {
    if (c == 'x')
      out += hex[dist(rd)];
    else if (c == 'y')
      out += hex[(dist(rd) & 0x3) | 0x8];
    else
      out += c;
  }
  return out;
}

// Moonlight only displays the MAC; a placeholder is fine.
static std::string first_mac() {
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1)
    return "00:00:00:00:00:00";
  std::string result = "00:00:00:00:00:00";
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  for (auto *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
      continue;
    if (std::string(ifa->ifa_name) == "lo")
      continue;
    struct ifreq ifr {};
    std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifa->ifa_name);
    if (sock >= 0 && ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
      auto *m = reinterpret_cast<unsigned char *>(ifr.ifr_hwaddr.sa_data);
      char buf[18];
      std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
      result = buf;
      break;
    }
  }
  if (sock >= 0)
    close(sock);
  freeifaddrs(ifaddr);
  return result;
}

AppState AppState::init(const std::string &state_dir) {
  fs::create_directories(state_dir);
  fs::create_directories(state_dir + "/clients");

  AppState s;
  s.state_dir = state_dir;
  s.cert_path = state_dir + "/cert.pem";
  s.key_path = state_dir + "/key.pem";

  if (!x509::cert_exists(s.key_path, s.cert_path)) {
    logs::log(logs::info, "Generating new server certificate at {}", s.cert_path);
    auto pkey = x509::generate_key();
    auto cert = x509::generate_x509(pkey);
    x509::write_to_disk(pkey, s.key_path, cert, s.cert_path);
  }
  s.server_pkey = x509::pkey_from_file(s.key_path);
  s.server_cert = x509::cert_from_file(s.cert_path);

  auto uuid_path = state_dir + "/uuid";
  if (fs::exists(uuid_path)) {
    s.uuid = read_file(uuid_path);
  } else {
    s.uuid = gen_uuid();
    write_file(uuid_path, s.uuid);
  }

  char host[256] = {0};
  gethostname(host, sizeof(host) - 1);
  s.hostname = host[0] ? host : "steam-stream";
  s.mac_address = first_mac();

  s.display_modes = {{1920, 1080, 60}, {1280, 720, 60}};
  s.apps = {{"Steam Big Picture", "1", false}, {"Steam Desktop", "2", false}};

  for (auto &entry : fs::directory_iterator(state_dir + "/clients")) {
    if (entry.path().extension() == ".pem") {
      auto pem = read_file(entry.path().string());
      if (!pem.empty())
        s.paired_clients_.push_back(pem);
    }
  }
  logs::log(logs::info, "Loaded {} paired client(s) from {}", s.paired_clients_.size(), state_dir + "/clients");

  return s;
}

std::optional<PairCache> AppState::get_pair_cache(const std::string &key) {
  std::lock_guard<std::mutex> lk(*mtx_);
  auto it = pairing_cache_.find(key);
  if (it == pairing_cache_.end())
    return std::nullopt;
  return it->second;
}

void AppState::set_pair_cache(const std::string &key, const PairCache &v) {
  std::lock_guard<std::mutex> lk(*mtx_);
  pairing_cache_[key] = v;
}

void AppState::remove_pair_cache(const std::string &key) {
  std::lock_guard<std::mutex> lk(*mtx_);
  pairing_cache_.erase(key);
}

void AppState::add_paired_client(const std::string &client_cert_pem) {
  std::lock_guard<std::mutex> lk(*mtx_);
  paired_clients_.push_back(client_cert_pem);
  auto fname = state_dir + "/clients/" + std::to_string(std::hash<std::string>{}(client_cert_pem)) + ".pem";
  write_file(fname, client_cert_pem);
}

void AppState::remove_paired_client(const std::string &client_cert_pem) {
  std::lock_guard<std::mutex> lk(*mtx_);
  paired_clients_.erase(std::remove(paired_clients_.begin(), paired_clients_.end(), client_cert_pem),
                        paired_clients_.end());
  auto fname = state_dir + "/clients/" + std::to_string(std::hash<std::string>{}(client_cert_pem)) + ".pem";
  std::error_code ec;
  fs::remove(fname, ec);
}

std::vector<std::string> AppState::paired_certs() {
  std::lock_guard<std::mutex> lk(*mtx_);
  return paired_clients_;
}

std::optional<PairedClient> AppState::get_client_via_ssl(const x509::x509_ptr &client_cert) {
  if (!client_cert)
    return std::nullopt;
  for (auto &pem : paired_certs()) {
    auto paired_cert = x509::cert_from_string(pem);
    auto err = x509::verification_error(paired_cert, client_cert);
    if (!err) {
      return PairedClient{.client_cert = pem};
    } else {
      logs::log(logs::trace, "X509 verification error: {}", err.value());
    }
  }
  return std::nullopt;
}

} // namespace state
