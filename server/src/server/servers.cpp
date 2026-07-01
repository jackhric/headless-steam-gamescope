#include <array>
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <server/https_server.hpp>
#include <server/rest_helpers.hpp>
#include <server/servers.hpp>
#include <server/session.hpp>
#include <server/state.hpp>
#include <thread>

using namespace std::chrono_literals;
namespace bt = boost::property_tree;

namespace HTTPServers {

struct PendingPin {
  std::shared_ptr<std::promise<std::string>> pin;
  std::string client_ip;
};
static std::mutex g_pin_mtx;
static std::map<std::string, PendingPin> g_pending_pins;

static constexpr const char *PIN_HTML = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>steam-stream pairing</title>
<style>body{font-family:sans-serif;max-width:30em;margin:3em auto}input{font-size:1.2em;padding:.3em}</style></head>
<body><h2>Enter the PIN shown by Moonlight</h2>
<form id="f"><input id="pin" placeholder="0000" autofocus> <button>Pair</button></form>
<p id="msg"></p>
<script>
const s=location.hash.slice(1);
f.onsubmit=async e=>{e.preventDefault();
 const r=await fetch('/pin/',{method:'POST',headers:{'Content-Type':'application/json'},
   body:JSON.stringify({pin:pin.value,secret:s})});
 msg.textContent=r.ok?'Sent. Check Moonlight.':'Error: '+await r.text();};
</script></body></html>)HTML";

template <class T>
static void serverinfo(const std::shared_ptr<typename SimpleWeb::Server<T>::Response> &response,
                       const std::shared_ptr<typename SimpleWeb::Server<T>::Request> &request,
                       state::AppState &state,
                       bool is_paired) {
  log_req<T>(request);
  bool is_https = std::is_same_v<SimpleWeb::HTTPS, T>;
  auto local_ip = request->local_endpoint().address().to_string();

  auto running = state.sessions->get_running();
  auto xml = moonlight::serverinfo(running != nullptr, // is_busy
                                   running ? 1 : 0,    // currentgame (app id placeholder)
                                   state.https_port,
                                   state.http_port,
                                   state.uuid,
                                   state.hostname,
                                   state.mac_address,
                                   local_ip,
                                   state.display_modes,
                                   is_paired ? 1 : 0,
                                   state.support_hevc,
                                   state.support_av1);
  send_xml<T>(response, SimpleWeb::StatusCode::success_ok, xml);
}

static std::string env_or(const char *k, const std::string &def) {
  const char *v = std::getenv(k);
  return v ? std::string(v) : def;
}

static std::shared_ptr<session::StreamSession>
create_launch_session(state::AppState &state, const SimpleWeb::CaseInsensitiveMultimap &headers,
                      const std::string &client_ip) {
  auto sess = std::make_shared<session::StreamSession>();
  sess->session_id = state.sessions->next_id();
  sess->client_ip = client_ip;

  sess->app_id = get_header(headers, "appid").value_or("1");
  for (const auto &a : state.apps)
    if (a.id == sess->app_id)
      sess->app_name = a.title;

  // mode = "WIDTHxHEIGHTxFPS"
  auto mode = get_header(headers, "mode").value_or("1920x1080x60");
  int w = 1920, h = 1080, fps = 60;
  std::sscanf(mode.c_str(), "%dx%dx%d", &w, &h, &fps);
  sess->client_width = w;
  sess->client_height = h;
  sess->client_fps = fps;

  // surroundAudioInfo: low 16 bits = channel count.
  int surround = std::stoi(get_header(headers, "surroundAudioInfo").value_or("196610"));
  sess->audio_channel_count = surround & 0xffff;

  // rikey is the AES-128 key (hex), rikeyid the IV seed (decimal).
  sess->aes_key = get_header(headers, "rikey").value_or("");
  sess->aes_iv = get_header(headers, "rikeyid").value_or("");
  if (sess->aes_key.empty() || sess->aes_iv.empty())
    logs::log(logs::warning, "[HTTP] launch missing rikey/rikeyid -- media encryption will fail");

  sess->hevc_supported = state.support_hevc;
  sess->av1_supported = state.support_av1;
  sess->video_stream_port = state.video_stream_port;
  sess->audio_stream_port = state.audio_stream_port;
  sess->control_stream_port = state.control_stream_port;

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> printable(33, 126);
  for (auto &c : sess->rtp_secret_payload)
    c = static_cast<char>(printable(gen));
  std::uniform_int_distribution<std::uint32_t> u32(0, UINT32_MAX);
  sess->enet_secret_payload = u32(gen);
  std::uniform_int_distribution<> octet(0, 255);
  sess->rtsp_fake_ip =
      std::to_string(octet(gen)) + "." + std::to_string(octet(gen)) + "." + std::to_string(octet(gen)) + "." +
      std::to_string(octet(gen));
  return sess;
}

static void http_pair(const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTP>::Response> &response,
                      const std::shared_ptr<typename SimpleWeb::Server<SimpleWeb::HTTP>::Request> &request,
                      state::AppState &state) {
  log_req<SimpleWeb::HTTP>(request);
  auto headers = request->parse_query_string();
  auto salt = get_header(headers, "salt");
  auto client_cert_str = get_header(headers, "clientcert");
  auto client_id = get_header(headers, "uniqueid");
  auto client_ip = request->remote_endpoint().address().to_string();

  if (!client_id) {
    send_xml<SimpleWeb::HTTP>(response,
                              SimpleWeb::StatusCode::client_error_bad_request,
                              fail_pair("Received pair request without uniqueid, stopping."));
    return;
  }
  auto cache_key = client_id.value() + "@" + client_ip;

  // Client sends salt + cert; we need the user PIN to derive the AES key.
  if (salt && client_cert_str) {
    if (state.get_pair_cache(cache_key)) {
      send_xml<SimpleWeb::HTTP>(response,
                                SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Out of order pair request (phase 1)"));
      state.remove_pair_cache(cache_key);
      return;
    }
    auto pin_promise = std::make_shared<std::promise<std::string>>();
    auto secret = crypto::str_to_hex(crypto::random(8));
    {
      std::lock_guard<std::mutex> lk(g_pin_mtx);
      for (auto it = g_pending_pins.begin(); it != g_pending_pins.end();) {
        if (it->second.client_ip == client_ip)
          it = g_pending_pins.erase(it);
        else
          ++it;
      }
      g_pending_pins[secret] = {pin_promise, client_ip};
    }
    logs::log(logs::info, ">>> PAIRING: open http://{}:{}/pin/#{} and enter the Moonlight PIN",
              request->local_endpoint().address().to_string(), state.http_port, secret);

    std::thread([response, pin_promise, secret, salt = salt.value(),
                 client_cert_str = client_cert_str.value(), cache_key, &state]() {
      auto fut = pin_promise->get_future();
      if (fut.wait_for(180s) != std::future_status::ready) {
        std::lock_guard<std::mutex> lk(g_pin_mtx);
        g_pending_pins.erase(secret);
        send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                  fail_pair("Timed out waiting for PIN"));
        return;
      }
      auto pin = fut.get();
      auto server_pem = x509::get_cert_pem(state.server_cert);
      auto [xml, aes_key] = moonlight::pair::get_server_cert(pin, salt, server_pem);

      state::PairCache pc;
      pc.client_cert = crypto::hex_to_str(client_cert_str, true);
      pc.aes_key = aes_key;
      pc.last_phase = state::PAIR_PHASE::GETSERVERCERT;
      state.set_pair_cache(cache_key, pc);

      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::success_ok, xml);
    }).detach();
    return;
  }

  auto cache_opt = state.get_pair_cache(cache_key);
  if (!cache_opt) {
    send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                              fail_pair("Unable to find " + cache_key + " in the pairing cache"));
    return;
  }
  auto cache = cache_opt.value();

  auto client_challenge = get_header(headers, "clientchallenge");
  if (client_challenge) {
    if (cache.last_phase != state::PAIR_PHASE::GETSERVERCERT) {
      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Out of order pair request (phase 2)"));
      state.remove_pair_cache(cache_key);
      return;
    }
    cache.last_phase = state::PAIR_PHASE::CLIENTCHALLENGE;
    auto server_cert_signature = x509::get_cert_signature(state.server_cert);
    auto [xml, server_secret_pair] =
        moonlight::pair::send_server_challenge(cache.aes_key, client_challenge.value(), server_cert_signature);
    cache.server_secret = server_secret_pair.first;
    cache.server_challenge = server_secret_pair.second;
    state.set_pair_cache(cache_key, cache);
    send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::success_ok, xml);
    return;
  }

  auto server_challenge = get_header(headers, "serverchallengeresp");
  if (server_challenge && cache.server_secret) {
    if (cache.last_phase != state::PAIR_PHASE::CLIENTCHALLENGE) {
      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Out of order pair request (phase 3)"));
      state.remove_pair_cache(cache_key);
      return;
    }
    cache.last_phase = state::PAIR_PHASE::SERVERCHALLENGERESP;
    auto [xml, client_hash] = moonlight::pair::get_client_hash(
        cache.aes_key, cache.server_secret.value(), server_challenge.value(),
        x509::get_pkey_content(state.server_pkey));
    cache.client_hash = client_hash;
    state.set_pair_cache(cache_key, cache);
    send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::success_ok, xml);
    return;
  }

  auto client_secret = get_header(headers, "clientpairingsecret");
  if (client_secret && cache.server_challenge && cache.client_hash) {
    if (cache.last_phase != state::PAIR_PHASE::SERVERCHALLENGERESP) {
      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Out of order pair request (phase 4)"));
      state.remove_pair_cache(cache_key);
      return;
    }
    auto client_cert = x509::cert_from_string(cache.client_cert);
    if (!client_cert) {
      send_xml<SimpleWeb::HTTP>(response, SimpleWeb::StatusCode::client_error_bad_request,
                                fail_pair("Unable to parse client certificate"));
      state.remove_pair_cache(cache_key);
      return;
    }
    auto client_sig = x509::get_cert_signature(client_cert);
    auto public_key = x509::get_cert_public_key(client_cert);
    auto xml = moonlight::pair::client_pair(cache.aes_key, cache.server_challenge.value(),
                                            cache.client_hash.value(), client_secret.value(),
                                            client_sig, public_key);
    bool paired = xml.get<int>("root.paired") == 1;
    send_xml<SimpleWeb::HTTP>(response,
                              paired ? SimpleWeb::StatusCode::success_ok
                                     : SimpleWeb::StatusCode::client_error_bad_request,
                              xml);
    if (paired) {
      state.add_paired_client(cache.client_cert);
      logs::log(logs::info, "Successfully paired {}", client_ip);
    } else {
      logs::log(logs::warning, "Failed pairing with {}", client_ip);
    }
    state.remove_pair_cache(cache_key);
    return;
  }

  logs::log(logs::warning, "Unable to match pair with any phase, retry pairing from Moonlight");
}

void start_http(state::AppState &state) {
  auto server = std::make_shared<HttpServer>();
  server->config.port = state.http_port;
  server->config.address = "0.0.0.0";
  server->default_resource["GET"] = [](auto resp, auto req) {
    send_xml<SimpleWeb::HTTP>(resp, SimpleWeb::StatusCode::client_error_not_found, fail_pair("not found"));
  };

  server->resource["^/serverinfo$"]["GET"] = [&state](auto resp, auto req) {
    serverinfo<SimpleWeb::HTTP>(resp, req, state, false);
  };
  server->resource["^/pair$"]["GET"] = [&state](auto resp, auto req) { http_pair(resp, req, state); };

  server->resource["^/pin/?$"]["GET"] = [](auto resp, auto req) { resp->write(PIN_HTML); };
  server->resource["^/pin/?$"]["POST"] = [](auto resp, auto req) {
    try {
      bt::ptree pt;
      read_json(req->content, pt);
      auto pin = pt.get<std::string>("pin");
      auto secret = pt.get<std::string>("secret");
      std::lock_guard<std::mutex> lk(g_pin_mtx);
      auto it = g_pending_pins.find(secret);
      if (it == g_pending_pins.end()) {
        *resp << "HTTP/1.1 404 Not Found\r\nContent-Length: 14\r\n\r\nunknown secret";
        return;
      }
      it->second.pin->set_value(pin);
      g_pending_pins.erase(it);
      resp->write("OK");
    } catch (const std::exception &e) {
      *resp << "HTTP/1.1 400 Bad Request\r\nContent-Length: " << strlen(e.what()) << "\r\n\r\n" << e.what();
    }
  };

  server->resource["^/unpair$"]["GET"] = [&state](auto resp, auto req) {
    auto headers = req->parse_query_string();
    auto client_id = get_header(headers, "uniqueid");
    auto client_ip = req->remote_endpoint().address().to_string();
    if (client_id) {
      auto cache = state.get_pair_cache(client_id.value() + "@" + client_ip);
      if (cache)
        state.remove_paired_client(cache->client_cert);
    }
    logs::log(logs::info, "Unpair request from {}", client_ip);
    XML xml;
    xml.put("root.<xmlattr>.status_code", 200);
    send_xml<SimpleWeb::HTTP>(resp, SimpleWeb::StatusCode::success_ok, xml);
  };

  logs::log(logs::info, "HTTP server listening on {}", state.http_port);
  server->start();
}

static std::optional<state::PairedClient>
client_if_paired(state::AppState &state, const std::shared_ptr<HttpsServer::Request> &req) {
  return state.get_client_via_ssl(HttpsServer::get_client_cert(req));
}

static void reply_unauthorized(const std::shared_ptr<HttpsServer::Response> &resp,
                               const std::shared_ptr<HttpsServer::Request> &req) {
  logs::log(logs::warning, "HTTPS request from an unpaired client: {}", req->path);
  XML xml;
  xml.put("root.<xmlattr>.status_code", 401);
  xml.put("root.<xmlattr>.query", req->path);
  xml.put("root.<xmlattr>.status_message", "The client is not authorized. Certificate verification failed.");
  send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::client_error_unauthorized, xml);
}

void start_https(state::AppState &state) {
  auto server = std::make_shared<HttpsServer>(state.cert_path, state.key_path);
  server->config.port = state.https_port;
  server->config.address = "0.0.0.0";
  server->default_resource["GET"] = [](auto resp, auto req) {
    send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::client_error_not_found, fail_pair("not found"));
  };

  server->resource["^/serverinfo$"]["GET"] = [&state](auto resp, auto req) {
    bool paired = client_if_paired(state, req).has_value();
    if (paired)
      serverinfo<SimpleWeb::HTTPS>(resp, req, state, true);
    else
      reply_unauthorized(resp, req);
  };

  server->resource["^/pair$"]["GET"] = [&state](auto resp, auto req) {
    if (client_if_paired(state, req)) {
      auto headers = req->parse_query_string();
      auto phrase = get_header(headers, "phrase");
      if (phrase && phrase.value() == "pairchallenge") {
        XML xml;
        xml.put("root.paired", 1);
        xml.put("root.<xmlattr>.status_code", 200);
        send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::success_ok, xml);
      }
    } else {
      reply_unauthorized(resp, req);
    }
  };

  server->resource["^/applist$"]["GET"] = [&state](auto resp, auto req) {
    if (client_if_paired(state, req)) {
      log_req<SimpleWeb::HTTPS>(req);
      send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::success_ok, moonlight::applist(state.apps));
    } else {
      reply_unauthorized(resp, req);
    }
  };

  auto launch_like = [&state](const std::shared_ptr<HttpsServer::Response> &resp,
                              const std::shared_ptr<HttpsServer::Request> &req, bool resume) {
    if (!client_if_paired(state, req)) {
      reply_unauthorized(resp, req);
      return;
    }
    log_req<SimpleWeb::HTTPS>(req);
    auto headers = req->parse_query_string();
    auto client_ip = req->remote_endpoint().address().to_string();
    auto local_ip = req->local_endpoint().address().to_string();
    auto port = std::to_string(state.rtsp_port);

    std::shared_ptr<session::StreamSession> sess;
    if (resume) {
      sess = state.sessions->get_by_client_ip(client_ip);
      // A resume can arrive from a different device (IP won't match); fall back to the single
      // running session and adopt the new client.
      if (!sess)
        sess = state.sessions->get_running();
      if (!sess) {
        logs::log(logs::warning, "[HTTP] resume with no existing session for {}", client_ip);
        send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::client_error_bad_request, fail_pair("no session"));
        return;
      }
      if (sess->client_ip != client_ip) {
        logs::log(logs::info, "[HTTP] resume: session {} moving client {} -> {}", sess->session_id,
                  sess->client_ip, client_ip);
        std::lock_guard<std::mutex> lk(*sess->mtx);
        sess->client_ip = client_ip;
      }
      // Resume rotates the per-session AES key; re-capture the new rikey/rikeyid or control +
      // audio keep decrypting with the stale key and flood failures. Don't clobber a working
      // key if the resume omits them.
      auto new_key = get_header(headers, "rikey").value_or("");
      auto new_iv = get_header(headers, "rikeyid").value_or("");
      if (!new_key.empty() && !new_iv.empty()) {
        std::lock_guard<std::mutex> lk(*sess->mtx);
        logs::log(logs::info, "[HTTP] resume session {}: AES key {} (rikey {} -> {}, rikeyid {} -> {})",
                  sess->session_id, sess->aes_key == new_key ? "UNCHANGED" : "ROTATED",
                  sess->aes_key, new_key, sess->aes_iv, new_iv);
        sess->aes_key = new_key;
        sess->aes_iv = new_iv;
      } else {
        logs::log(logs::warning, "[HTTP] resume for session {} missing rikey/rikeyid -- keeping existing key",
                  sess->session_id);
      }
    } else {
      // Already streaming? Re-launching would clobber the live session; resume instead.
      sess = state.sessions->get_by_client_ip(client_ip);
      if (!sess) {
        sess = create_launch_session(state, headers, client_ip);
        state.sessions->add(sess);
        logs::log(logs::info, "[HTTP] launch: created session {} for {} ({}x{}@{}), rtsp_fake_ip={}",
                  sess->session_id, client_ip, sess->client_width, sess->client_height, sess->client_fps,
                  sess->rtsp_fake_ip);
      } else {
        logs::log(logs::info, "[HTTP] launch: client {} already has session {}, resuming", client_ip,
                  sess->session_id);
      }
    }

    // Hand back the per-session fake IP so the RTSP thread can re-identify the session
    // (Moonlight parrots it into the RTSP URI/Host).
    bool use_fake_ip = env_or("STEAM_STREAM_RTSP_FAKE_IP", "TRUE") == "TRUE";
    auto rtsp_host = use_fake_ip ? sess->rtsp_fake_ip : local_ip;
    auto xml = resume ? moonlight::launch_resume(rtsp_host, port) : moonlight::launch_success(rtsp_host, port);
    send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::success_ok, xml);
  };
  server->resource["^/launch"]["GET"] = [launch_like](auto resp, auto req) { launch_like(resp, req, false); };
  server->resource["^/resume"]["GET"] = [launch_like](auto resp, auto req) { launch_like(resp, req, true); };

  server->resource["^/cancel"]["GET"] = [&state](auto resp, auto req) {
    if (client_if_paired(state, req)) {
      log_req<SimpleWeb::HTTPS>(req);
      auto client_ip = req->remote_endpoint().address().to_string();
      if (auto sess = state.sessions->get_by_client_ip(client_ip)) {
        state.sessions->remove(sess->session_id);
        logs::log(logs::info, "[HTTP] cancel: removed session {} for {}", sess->session_id, client_ip);
      }
      XML xml;
      xml.put("root.<xmlattr>.status_code", 200);
      xml.put("root.cancel", 1);
      send_xml<SimpleWeb::HTTPS>(resp, SimpleWeb::StatusCode::success_ok, xml);
    } else {
      reply_unauthorized(resp, req);
    }
  };

  logs::log(logs::info, "HTTPS server listening on {}", state.https_port);
  server->start();
}

} // namespace HTTPServers
