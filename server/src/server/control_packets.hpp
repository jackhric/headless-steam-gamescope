#pragma once

// Moonlight control-stream packet layout + AES-GCM decrypt. Layout and the GCM IV scheme
// (seq in byte 0, 16-byte tag) are byte-identical to the wire format.

#include <array>
#include <boost/endian/conversion.hpp>
#include <cstdint>
#include <crypto/crypto.hpp>
#include <string>
#include <string_view>

namespace control {

namespace pkts {

enum PACKET_TYPE : std::uint16_t {
  INPUT_DATA = boost::endian::little_to_native(0x0206),
  TERMINATION = boost::endian::little_to_native(0x0109),
  PERIODIC_PING = boost::endian::little_to_native(0x0200),
  IDR_FRAME = boost::endian::little_to_native(0x0302),
  ENCRYPTED = boost::endian::little_to_native(0x0001),
};

enum INPUT_TYPE : int {
  MOUSE_MOVE_REL = boost::endian::native_to_little(0x00000007),
  MOUSE_MOVE_ABS = boost::endian::native_to_little(0x00000005),
  MOUSE_BUTTON_PRESS = boost::endian::native_to_little(0x00000008),
  MOUSE_BUTTON_RELEASE = boost::endian::native_to_little(0x00000009),
  KEY_PRESS = boost::endian::native_to_little(0x00000003),
  KEY_RELEASE = boost::endian::native_to_little(0x00000004),
  MOUSE_SCROLL = boost::endian::native_to_little(0x0000000A),
  MOUSE_HSCROLL = boost::endian::native_to_little(0x55000001),
  CONTROLLER_MULTI = boost::endian::native_to_little(0x0000000C),
};

enum KEYBOARD_MODIFIERS : char { NONE = 0x00, SHIFT = 0x01, CTRL = 0x02, ALT = 0x04, META = 0x08 };
enum MOONLIGHT_MODIFIERS : short { M_SHIFT = 0x10, M_CTRL = 0x11, M_ALT = 0xA4, M_META = 0x5B };

#pragma pack(push, 1)

struct INPUT_PKT {
  unsigned short packet_type;
  unsigned short packet_len;
  unsigned int data_size;
  INPUT_TYPE type;
};

struct MOUSE_MOVE_REL_PACKET : INPUT_PKT {
  short delta_x;
  short delta_y;
};

struct MOUSE_MOVE_ABS_PACKET : INPUT_PKT {
  short x;
  short y;
  short unused;
  short width;
  short height;
};

struct MOUSE_BUTTON_PACKET : INPUT_PKT {
  unsigned char button;
};

struct MOUSE_SCROLL_PACKET : INPUT_PKT {
  short scroll_amt1;
  short scroll_amt2;
  short zero1;
};

struct MOUSE_HSCROLL_PACKET : INPUT_PKT {
  short scroll_amount;
};

struct KEYBOARD_PACKET : INPUT_PKT {
  unsigned char flags;
  short key_code;
  char modifiers;
  short zero1;
};

struct CONTROLLER_MULTI_PACKET : INPUT_PKT {
  short header_b;
  short controller_number;
  short active_gamepad_mask;
  short mid_b;
  short button_flags;
  unsigned char left_trigger;
  unsigned char right_trigger;
  short left_stick_x;
  short left_stick_y;
  short right_stick_x;
  short right_stick_y;
  short tail_a;
  short buttonFlags2;
  short tailB;
};

#pragma pack(pop)

} // namespace pkts

static constexpr int GCM_TAG_SIZE = 16;
static constexpr int MAX_PAYLOAD_SIZE = 128;

struct ControlPacket {
  pkts::PACKET_TYPE type;
  std::uint16_t length;
};

struct ControlEncryptedPacket {
  ControlPacket header;
  std::uint32_t seq;
  char gcm_tag[GCM_TAG_SIZE];
  char payload[MAX_PAYLOAD_SIZE];

  [[nodiscard]] std::string_view encrypted_msg() const {
    auto len = boost::endian::little_to_native(this->header.length);
    return {payload, static_cast<size_t>(len - GCM_TAG_SIZE - sizeof(seq))};
  }
};

// IV = 16 zero bytes with byte 0 = seq (little-endian).
inline std::string decrypt_packet(const ControlEncryptedPacket &pkt, std::string_view gcm_key_hex) {
  std::array<std::uint8_t, GCM_TAG_SIZE> iv_data = {0};
  iv_data[0] = boost::endian::little_to_native(pkt.seq);
  return crypto::aes_decrypt_gcm(pkt.encrypted_msg(),
                                 crypto::hex_to_str(gcm_key_hex.data(), true),
                                 pkt.gcm_tag,
                                 {(char *)iv_data.data(), iv_data.size()},
                                 GCM_TAG_SIZE);
}

inline const char *packet_type_to_str(pkts::PACKET_TYPE p) noexcept {
  switch (p) {
  case pkts::INPUT_DATA: return "INPUT_DATA";
  case pkts::TERMINATION: return "TERMINATION";
  case pkts::PERIODIC_PING: return "PERIODIC_PING";
  case pkts::IDR_FRAME: return "IDR_FRAME";
  case pkts::ENCRYPTED: return "ENCRYPTED";
  }
  return "Unrecognised";
}

} // namespace control
