#pragma once

#include <boost/asio/ssl.hpp>
#include <crypto/crypto.hpp>
#include <server_http.hpp>

// verify_callback must always return 1: Moonlight clients present a self-signed cert, so we
// complete the handshake regardless and authenticate per-endpoint against the paired-client
// store. (verify_none never requests a client cert, so we couldn't read it back.)

namespace SimpleWeb {

using HTTPS = asio::ssl::stream<asio::ip::tcp::socket>;

template <> class Server<HTTPS> : public ServerBase<HTTPS> {
  bool set_session_id_context = false;

protected:
  asio::ssl::context context;
  void after_bind() override;
  void accept() override;

public:
  Server(const std::string &certification_file, const std::string &private_key_file);
  static x509::x509_ptr get_client_cert(const std::shared_ptr<typename ServerBase<HTTPS>::Request> &request);
};
} // namespace SimpleWeb

using HttpsServer = SimpleWeb::Server<SimpleWeb::HTTPS>;
using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;
