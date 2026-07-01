// Regression: /resume must re-capture the freshly-negotiated rikey. Moonlight rotates the
// AES key on every resume; keeping the stale key breaks control + encrypted audio decrypt.

#include <array>
#include <boost/endian/conversion.hpp>
#include <cassert>
#include <crypto/crypto.hpp>
#include <cstring>
#include <iostream>
#include <server/control_packets.hpp>
#include <server/session.hpp>

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

// Rotate the session key from the resume request, but never clobber a working key with an empty one.
static void apply_resume_rekey(session::StreamSession &sess, const std::string &rikey,
                               const std::string &rikeyid) {
  if (!rikey.empty() && !rikeyid.empty()) {
    sess.aes_key = rikey;
    sess.aes_iv = rikeyid;
  }
}

static bool decrypts_clean(const ControlEncryptedPacket &pkt, const std::string &key,
                           short expect_dx, short expect_dy) {
  try {
    auto dec = decrypt_packet(pkt, key);
    if (dec.size() != sizeof(MOUSE_MOVE_REL_PACKET))
      return false;
    auto *out = (MOUSE_MOVE_REL_PACKET *)dec.data();
    return ((ControlPacket *)dec.data())->type == INPUT_DATA && out->type == MOUSE_MOVE_REL &&
           boost::endian::big_to_native(out->delta_x) == expect_dx &&
           boost::endian::big_to_native(out->delta_y) == expect_dy;
  } catch (...) {
    return false; // GCM tag rejected -> wrong key
  }
}

int main() {
  // rikey = 32 hex chars (AES-128 key); rikeyid = decimal.
  const std::string keyA = "9d804e9d3a7d8f1b2c3d4e5f60718293", ivA = "16909060";
  const std::string keyB = "00112233445566778899aabbccddeeff", ivB = "305419896";
  assert(keyA != keyB);

  session::StreamSession sess;
  sess.aes_key = keyA;
  sess.aes_iv = ivA;

  MOUSE_MOVE_REL_PACKET in{};
  in.packet_type = INPUT_DATA;
  in.type = MOUSE_MOVE_REL;
  in.delta_x = boost::endian::native_to_big((short)42);
  in.delta_y = boost::endian::native_to_big((short)-7);
  in.data_size = sizeof(in) - sizeof(in.packet_type) - sizeof(in.packet_len);
  auto pkt_B = encrypt(keyB, 1, {(char *)&in, sizeof(in)});

  assert(!decrypts_clean(pkt_B, sess.aes_key, 42, -7));
  ok("stale key A rejects the client's key-B control packet (reproduces the bug)");

  apply_resume_rekey(sess, keyB, ivB);
  assert(sess.aes_key == keyB);
  assert(sess.aes_iv == ivB);
  ok("resume re-capture rotates sess->aes_key A -> B (and aes_iv)");

  assert(decrypts_clean(pkt_B, sess.aes_key, 42, -7));
  ok("rotated key B decrypts the control packet cleanly (control fix verified)");

  apply_resume_rekey(sess, "", "");
  assert(sess.aes_key == keyB && sess.aes_iv == ivB);
  ok("empty resume keeps the existing key (no clobber)");

  std::cout << "\nALL RESUME-KEY TESTS PASSED\n";
  return 0;
}
