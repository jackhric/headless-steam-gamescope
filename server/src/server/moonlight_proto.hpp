#pragma once
// GameStream XML + PIN pairing helpers.

#include <boost/property_tree/ptree.hpp>
#include <crypto/crypto.hpp>
#include <openssl/aes.h>
#include <string>
#include <vector>

namespace moonlight {

namespace pt = boost::property_tree;
using XML = pt::ptree;

constexpr auto M_VERSION = "7.1.431.-1";
constexpr auto M_GFE_VERSION = "3.23.0.74";

struct DisplayMode {
  int width;
  int height;
  int refreshRate;
};

struct App {
  std::string title;
  std::string id;
  bool support_hdr = false;
};

XML serverinfo(bool isServerBusy,
               int current_appid,
               int https_port,
               int http_port,
               const std::string &uuid,
               const std::string &hostname,
               const std::string &mac_address,
               const std::string &local_ip,
               const std::vector<DisplayMode> &display_modes,
               int pair_status,
               bool support_hevc,
               bool support_av1);

namespace pair {

std::pair<XML, std::string>
get_server_cert(const std::string &user_pin, const std::string &salt, const std::string &server_cert_pem);

std::string gen_aes_key(const std::string &salt, const std::string &pin);

std::pair<XML, std::pair<std::string, std::string>>
send_server_challenge(const std::string &aes_key,
                      const std::string &client_challenge,
                      const std::string &server_cert_signature,
                      const std::string &server_secret = crypto::random(16),
                      const std::string &server_challenge = crypto::random(16));

std::pair<XML, std::string> get_client_hash(const std::string &aes_key,
                                            const std::string &server_secret,
                                            const std::string &server_challenge_resp,
                                            const std::string &server_cert_private_key);

XML client_pair(const std::string &aes_key,
                const std::string &server_challenge,
                const std::string &client_hash,
                const std::string &client_pairing_secret,
                const std::string &client_public_cert_signature,
                const std::string &client_cert_public_key);
} // namespace pair

XML applist(const std::vector<App> &apps);

XML launch_success(const std::string &local_ip, const std::string &rtsp_port);
XML launch_resume(const std::string &local_ip, const std::string &rtsp_port);

} // namespace moonlight
