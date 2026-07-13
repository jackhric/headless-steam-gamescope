#include <server/control.hpp>

#include <arpa/inet.h>
#include <boost/endian/conversion.hpp>
#include <enet/enet.h>
#include <helpers/logger.hpp>
#include <map>
#include <string>
#include <server/control_packets.hpp>
#include <server/media.hpp>
#include <server/uinput.hpp>

namespace control {

using namespace control::pkts;

namespace {

std::pair<std::string, int> get_ip(const sockaddr *addr) {
  char data[INET6_ADDRSTRLEN] = {0};
  int port = 0;
  if (addr->sa_family == AF_INET6) {
    inet_ntop(AF_INET6, &((sockaddr_in6 *)addr)->sin6_addr, data, INET6_ADDRSTRLEN);
    port = ntohs(((sockaddr_in6 *)addr)->sin6_port);
  } else if (addr->sa_family == AF_INET) {
    inet_ntop(AF_INET, &((sockaddr_in *)addr)->sin_addr, data, INET_ADDRSTRLEN);
    port = ntohs(((sockaddr_in *)addr)->sin_port);
  }
  return {std::string(data), port};
}

// Mouse/keyboard/scroll go through the compositor (waylanddisplaysrc); the gamepad is cold-plugged
// by the MediaSession at session start and driven via mm->gamepad_update.
struct ClientDevices {
  std::shared_ptr<session::StreamSession> session;
};

void handle_input(ClientDevices &dev, const INPUT_PKT *pkt,
                  const std::shared_ptr<media::MediaSession> &mm) {
  switch (pkt->type) {
  case MOUSE_MOVE_REL: {
    auto p = (const MOUSE_MOVE_REL_PACKET *)pkt;
    if (mm)
      mm->mouse_move_rel(boost::endian::big_to_native(p->delta_x),
                         boost::endian::big_to_native(p->delta_y));
    break;
  }
  case MOUSE_MOVE_ABS: {
    auto p = (const MOUSE_MOVE_ABS_PACKET *)pkt;
    float x = boost::endian::big_to_native(p->x);
    float y = boost::endian::big_to_native(p->y);
    float w = boost::endian::big_to_native(p->width);
    float h = boost::endian::big_to_native(p->height);
    int out_w = dev.session->video.width > 0 ? dev.session->video.width : (int)w;
    int out_h = dev.session->video.height > 0 ? dev.session->video.height : (int)h;
    if (mm && w > 0 && h > 0)
      mm->mouse_move_abs(x / w * out_w, y / h * out_h);
    break;
  }
  case MOUSE_BUTTON_PRESS:
  case MOUSE_BUTTON_RELEASE: {
    auto p = (const MOUSE_BUTTON_PACKET *)pkt;
    if (mm)
      mm->mouse_button(input::moonlight_button_to_linux(p->button),
                       pkt->type == MOUSE_BUTTON_PRESS);
    break;
  }
  case MOUSE_SCROLL: {
    auto p = (const MOUSE_SCROLL_PACKET *)pkt;
    if (mm) // negate to match the client's scroll direction
      mm->mouse_axis(0, -(double)boost::endian::big_to_native(p->scroll_amt1));
    break;
  }
  case MOUSE_HSCROLL: {
    auto p = (const MOUSE_HSCROLL_PACKET *)pkt;
    if (mm)
      mm->mouse_axis((double)boost::endian::big_to_native(p->scroll_amount), 0);
    break;
  }
  case KEY_PRESS:
  case KEY_RELEASE: {
    auto p = (const KEYBOARD_PACKET *)pkt;
    short key = (short)boost::endian::little_to_native(p->key_code) & (short)0x7fff;
    int linux_key = input::moonlight_key_to_linux(key);
    if (mm && linux_key >= 0)
      mm->keyboard_key((unsigned int)linux_key, pkt->type == KEY_PRESS);
    break;
  }
  case CONTROLLER_ARRIVAL: {
    auto p = (const CONTROLLER_ARRIVAL_PACKET *)pkt;
    if (mm)
      mm->gamepad_arrival(p->controller_number);
    break;
  }
  case CONTROLLER_MULTI: {
    auto p = (const CONTROLLER_MULTI_PACKET *)pkt;
    if (mm)
      mm->gamepad_update(
          boost::endian::little_to_native((std::uint16_t)p->controller_number),
          boost::endian::little_to_native((std::uint16_t)p->active_gamepad_mask),
          boost::endian::little_to_native((std::uint16_t)p->button_flags), p->left_trigger,
          p->right_trigger, boost::endian::little_to_native(p->left_stick_x),
          boost::endian::little_to_native(p->left_stick_y),
          boost::endian::little_to_native(p->right_stick_x),
          boost::endian::little_to_native(p->right_stick_y));
    break;
  }
  default:
    logs::log(logs::debug, "[ENET] unhandled input type 0x{:x}", (int)pkt->type);
  }
}

std::shared_ptr<session::StreamSession>
match_session(session::SessionRegistry &reg, std::uint32_t connect_data, const std::string &ip) {
  // Single-session design: get_running() + secret/IP check suffices.
  if (auto s = reg.get_running()) {
    if (s->enet_secret_payload == connect_data || s->client_ip == ip)
      return s;
  }
  return nullptr;
}

} // namespace

bool ControlServer::global_init() {
  if (enet_initialize() != 0) {
    logs::log(logs::error, "[ENET] enet_initialize failed");
    return false;
  }
  return true;
}

ControlServer::ControlServer(int port, std::shared_ptr<session::SessionRegistry> registry)
    : port_(port), registry_(std::move(registry)) {}

ControlServer::~ControlServer() { stop(); }

void ControlServer::set_idr_callback(std::function<void(std::size_t)> cb) { on_idr_ = std::move(cb); }

void ControlServer::set_stop_callback(std::function<void(std::size_t)> cb) { on_stop_ = std::move(cb); }

void ControlServer::set_media_accessor(std::function<std::shared_ptr<media::MediaSession>()> cb) {
  get_media_ = std::move(cb);
}

void ControlServer::stop() { running_ = false; }

void ControlServer::run() {
  ENetAddress addr;
  enet_address_set_host(&addr, "0.0.0.0");
  enet_address_set_port(&addr, port_);

  // Moonlight ENet fork: first arg is the address family.
  ENetHost *host = enet_host_create(AF_INET, &addr, 4, 0, 0, 0);
  if (!host) {
    logs::log(logs::error, "[ENET] failed to create control host on :{}", port_);
    return;
  }
  logs::log(logs::info, "[ENET] control server started on port {}", port_);

  std::map<ENetPeer *, ClientDevices> clients;
  running_ = true;
  ENetEvent event;

  while (running_) {
    if (enet_host_service(host, &event, 50) <= 0)
      continue;

    auto [client_ip, client_port] = get_ip((sockaddr *)&event.peer->address.address);

    switch (event.type) {
    case ENET_EVENT_TYPE_CONNECT: {
      auto sess = match_session(*registry_, event.data, client_ip);
      if (!sess) {
        logs::log(logs::warning, "[ENET] connect from {}:{} with no matching session (data={})",
                  client_ip, client_port, event.data);
        enet_peer_disconnect_now(event.peer, 0);
        break;
      }
      logs::log(logs::info, "[ENET] client connected {}:{} -> session {}", client_ip, client_port,
                sess->session_id);
      clients[event.peer] = ClientDevices{.session = sess};
      break;
    }
    case ENET_EVENT_TYPE_DISCONNECT:
      logs::log(logs::info, "[ENET] client disconnected {}:{}", client_ip, client_port);
      clients.erase(event.peer);
      break;
    case ENET_EVENT_TYPE_RECEIVE: {
      auto it = clients.find(event.peer);
      if (it == clients.end()) {
        enet_packet_destroy(event.packet);
        break;
      }
      auto &dev = it->second;
      auto type = ((ControlPacket *)event.packet->data)->type;
      if (type == ENCRYPTED) {
        try {
          auto *enc = (ControlEncryptedPacket *)event.packet->data;
          auto decrypted = decrypt_packet(*enc, dev.session->aes_key);
          auto sub_type = ((ControlPacket *)decrypted.data())->type;
          if (sub_type == INPUT_DATA) {
            handle_input(dev, (INPUT_PKT *)decrypted.data(), get_media_ ? get_media_() : nullptr);
          } else if (sub_type == IDR_FRAME) {
            if (on_idr_)
              on_idr_(dev.session->session_id);
          } else if (sub_type == TERMINATION) {
            logs::log(logs::info, "[ENET] client requested termination for session {}",
                      dev.session->session_id);
            if (on_stop_)
              on_stop_(dev.session->session_id);
          }
        } catch (const std::exception &e) {
          logs::log(logs::warning, "[ENET] failed to decrypt control packet: {}", e.what());
        }
      } else {
        logs::log(logs::trace, "[ENET] plaintext control packet {}", packet_type_to_str(type));
      }
      enet_packet_destroy(event.packet);
      break;
    }
    default:
      break;
    }
  }

  enet_host_destroy(host);
  logs::log(logs::info, "[ENET] control server stopped");
}

} // namespace control
