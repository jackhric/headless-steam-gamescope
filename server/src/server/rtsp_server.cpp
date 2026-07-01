#include <boost/asio.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/shared_ptr.hpp>
#include <chrono>
#include <helpers/logger.hpp>
#include <server/rtsp_commands.hpp>
#include <server/rtsp_server.hpp>
#include <string>

namespace rtsp {

namespace asio = boost::asio;
using asio::ip::tcp;

class tcp_connection : public boost::enable_shared_from_this<tcp_connection> {
public:
  using pointer = boost::shared_ptr<tcp_connection>;

  static pointer create(asio::io_context &io_context,
                        session::SessionRegistry &registry,
                        const std::function<void(session::StreamSession &)> &on_play) {
    return pointer(new tcp_connection(io_context, registry, on_play));
  }

  tcp::socket &socket() {
    return socket_;
  }

  void close() {
    boost::system::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);
  }

  void start() {
    boost::system::error_code ep_ec;
    auto remote = socket_.remote_endpoint(ep_ec);
    if (ep_ec) {
      logs::log(logs::debug, "[RTSP] connection gone before processing: {}", ep_ec.message());
      close();
      return;
    }
    std::string peer_ip = remote.address().to_string();
    logs::log(logs::trace, "[RTSP] connection from {}", peer_ip);
    receive_message([self = shared_from_this(), peer_ip](std::optional<RTSP_PACKET> parsed) {
      if (!parsed) {
        logs::log(logs::error, "[RTSP] error parsing message");
        self->send_message(error_msg(400, "BAD REQUEST"), [self](auto) { self->close(); });
        return;
      }
      std::string host_opt;
      if (auto it = parsed->options.find("Host"); it != parsed->options.end())
        host_opt = it->second;
      auto sess = self->registry_.get_for_rtsp(parsed->request.uri.ip, host_opt, peer_ip);
      if (!sess) {
        logs::log(logs::warning, "[RTSP] packet from unrecognised client: {} (uri ip '{}', host '{}')", peer_ip,
                  parsed->request.uri.ip, host_opt);
        self->close();
        return;
      }
      auto response = message_handler(*parsed, *sess, self->on_play_);
      self->send_message(response, [self](auto) { self->close(); });
    });
  }

  void receive_message(const std::function<void(std::optional<RTSP_PACKET>)> &on_msg_read) {
    deadline_.async_wait([self = shared_from_this()](auto error) {
      if (!error && self->deadline_.expiry() <= asio::steady_timer::clock_type::now()) {
        self->socket_.cancel();
        self->deadline_.cancel();
      }
    });
    deadline_.expires_after(std::chrono::milliseconds(timeout_millis));

    asio::async_read(
        socket_, streambuf_, asio::transfer_at_least(1),
        [self = shared_from_this(), on_msg_read](auto error_code, auto bytes_transferred) {
          if (error_code && error_code != asio::error::operation_aborted) {
            logs::log(logs::error, "[RTSP] error during transmission: {}", error_code.message());
            self->send_message(error_msg(400, "BAD REQUEST"), [](auto) {});
            return;
          }
          self->deadline_.cancel();
          std::string raw_msg = {std::istreambuf_iterator<char>(&self->streambuf_), {}};

          auto full = self->prev_read_ + raw_msg;
          auto total = self->prev_read_bytes_ + bytes_transferred;
          full.resize(total);

          // ANNOUNCE bodies exceed one read; keep reading until we have Content-length bytes.
          auto cl_pos = full.find("Content-length: ");
          if (cl_pos != std::string::npos) {
            cl_pos += 16;
            auto cl_end = full.find("\r\n", cl_pos);
            auto total_length = std::stoi(full.substr(cl_pos, cl_end - cl_pos)) + cl_end + 2;
            if (static_cast<long>(total) < total_length) {
              self->prev_read_ = full;
              self->prev_read_bytes_ += bytes_transferred;
              return self->receive_message(on_msg_read);
            }
          }

          auto msg = rtsp::parse(full);
          self->prev_read_.clear();
          self->prev_read_bytes_ = 0;
          on_msg_read(msg);
        });
  }

  void send_message(const RTSP_PACKET &response, const std::function<void(int)> &on_sent) {
    auto raw = rtsp::to_string(response);
    logs::log(logs::trace, "[RTSP] sending reply:\n{}", raw);
    auto buf = std::make_shared<std::string>(std::move(raw));
    asio::async_write(socket_, asio::buffer(*buf),
                      [buf, on_sent](auto error_code, auto bytes_transferred) {
                        if (error_code)
                          logs::log(logs::error, "[RTSP] error sending: {}", error_code.message());
                        on_sent(static_cast<int>(bytes_transferred));
                      });
  }

protected:
  tcp_connection(asio::io_context &io_context,
                 session::SessionRegistry &registry,
                 std::function<void(session::StreamSession &)> on_play)
      : socket_(io_context), streambuf_(max_msg_size), deadline_(io_context), registry_(registry),
        on_play_(std::move(on_play)) {}

  static constexpr auto max_msg_size = 4096;
  static constexpr auto timeout_millis = 2500;

  tcp::socket socket_;
  asio::streambuf streambuf_;
  asio::steady_timer deadline_;
  session::SessionRegistry &registry_;
  std::function<void(session::StreamSession &)> on_play_;
  std::string prev_read_;
  int prev_read_bytes_ = 0;
};

class tcp_server {
public:
  tcp_server(asio::io_context &io_context,
             int port,
             session::SessionRegistry &registry,
             std::function<void(session::StreamSession &)> on_play)
      : io_context_(io_context), acceptor_(io_context, tcp::endpoint(tcp::v4(), port)), registry_(registry),
        on_play_(std::move(on_play)) {
    acceptor_.set_option(asio::socket_base::reuse_address{true});
    acceptor_.listen(4096);
    start_accept();
  }

private:
  void start_accept() {
    auto conn = tcp_connection::create(io_context_, registry_, on_play_);
    acceptor_.async_accept(conn->socket(), [this, conn](auto error) { handle_accept(conn, error); });
  }

  void handle_accept(const tcp_connection::pointer &conn, const boost::system::error_code &error) {
    start_accept(); // re-arm first, so servicing failures can't stop the server
    if (!error) {
      try {
        conn->start();
      } catch (const std::exception &e) {
        logs::log(logs::warning, "[RTSP] error servicing connection: {}", e.what());
      }
    } else {
      logs::log(logs::error, "[RTSP] error during connection: {}", error.message());
    }
  }

  asio::io_context &io_context_;
  tcp::acceptor acceptor_;
  session::SessionRegistry &registry_;
  std::function<void(session::StreamSession &)> on_play_;
};

void run_server(int port,
                session::SessionRegistry &registry,
                std::function<void(session::StreamSession &)> on_play) {
  asio::io_context io_context;
  try {
    tcp_server server(io_context, port, registry, std::move(on_play));
    logs::log(logs::info, "RTSP server started on port: {}", port);
    while (true) {
      try {
        io_context.run();
        break;
      } catch (const std::exception &e) {
        logs::log(logs::warning, "[RTSP] connection handler exception: {} (resuming)", e.what());
      }
    }
  } catch (const std::exception &e) {
    logs::log(logs::error, "Unable to create RTSP server on port {}: {}", port, e.what());
  }
}

} // namespace rtsp
