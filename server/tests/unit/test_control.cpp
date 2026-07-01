// Control channel crypto + packet layout: encrypt an INPUT_DATA packet as a client
// would, decrypt it back, assert fields survive.

#include <array>
#include <boost/endian/conversion.hpp>
#include <cassert>
#include <crypto/crypto.hpp>
#include <cstring>
#include <iostream>
#include <server/control_packets.hpp>

using namespace control;
using namespace control::pkts;

static void ok(const char *what) { std::cout << "[ OK ] " << what << "\n"; }

static ControlEncryptedPacket encrypt(std::string_view key_hex, std::uint32_t seq, std::string_view payload) {
  std::array<std::uint8_t, GCM_TAG_SIZE> iv = {0};
  iv[0] = boost::endian::native_to_little(seq);
  auto [enc, tag] = crypto::aes_encrypt_gcm(payload, crypto::hex_to_str(key_hex.data(), true),
                                            {(char *)iv.data(), iv.size()}, GCM_TAG_SIZE);
  ControlEncryptedPacket pkt{};
  pkt.header.type = ENCRYPTED;
  pkt.header.length =
      boost::endian::native_to_little((std::uint16_t)(sizeof(seq) + GCM_TAG_SIZE + enc.length()));
  pkt.seq = boost::endian::native_to_little(seq);
  std::memcpy(pkt.gcm_tag, tag.data(), GCM_TAG_SIZE);
  std::memcpy(pkt.payload, enc.data(), enc.size());
  return pkt;
}

int main() {
  const std::string rikey = "9d804e9d3a7d8f1b2c3d4e5f60718293";

  MOUSE_MOVE_REL_PACKET in{};
  in.packet_type = INPUT_DATA;
  in.type = MOUSE_MOVE_REL;
  in.delta_x = boost::endian::native_to_big((short)42);
  in.delta_y = boost::endian::native_to_big((short)-7);
  in.data_size = sizeof(in) - sizeof(in.packet_type) - sizeof(in.packet_len);

  auto enc = encrypt(rikey, 1, {(char *)&in, sizeof(in)});
  auto dec = decrypt_packet(enc, rikey);
  assert(dec.size() == sizeof(in));

  auto *base = (ControlPacket *)dec.data();
  assert(base->type == INPUT_DATA);
  auto *out = (MOUSE_MOVE_REL_PACKET *)dec.data();
  assert(out->type == MOUSE_MOVE_REL);
  assert(boost::endian::big_to_native(out->delta_x) == 42);
  assert(boost::endian::big_to_native(out->delta_y) == -7);
  ok("MOUSE_MOVE_REL decrypts + fields intact");

  KEYBOARD_PACKET kb{};
  kb.packet_type = INPUT_DATA;
  kb.type = KEY_PRESS;
  kb.key_code = boost::endian::native_to_little((short)0x41); // 'A'
  kb.modifiers = CTRL;
  auto kenc = encrypt(rikey, 2, {(char *)&kb, sizeof(kb)});
  auto kdec = decrypt_packet(kenc, rikey);
  auto *kout = (KEYBOARD_PACKET *)kdec.data();
  assert(kout->type == KEY_PRESS);
  assert(((short)boost::endian::little_to_native(kout->key_code) & 0x7fff) == 0x41);
  ok("KEY_PRESS decrypts + keycode intact");

  bool threw = false;
  try {
    decrypt_packet(enc, "00000000000000000000000000000000");
  } catch (...) {
    threw = true;
  }
  assert(threw);
  ok("GCM tag rejects wrong key");

  std::cout << "\nALL CONTROL TESTS PASSED\n";
  return 0;
}
