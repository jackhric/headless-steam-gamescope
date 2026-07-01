// AES roundtrip: control channel (GCM-128) and audio RTP payload (CBC-128).

#include <array>
#include <boost/endian/conversion.hpp>
#include <cstdint>
#include <cstdio>
#include <crypto/crypto.hpp>
#include <string>

static int failures = 0;
#define CHECK(cond, name)                                                                                              \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::printf("  [FAIL] %s\n", name);                                                                              \
      failures++;                                                                                                      \
    } else {                                                                                                           \
      std::printf("  [ OK ] %s\n", name);                                                                             \
    }                                                                                                                  \
  } while (0)

int main() {
  const std::string rikey = "9d804e9d3a7d8f1b2c3d4e5f60718293";
  const std::string key = crypto::hex_to_str(rikey, true);

  std::printf("== AES-CBC-128 roundtrip (audio RTP payload path) ==\n");
  {
    CHECK(key.size() == 16, "rikey hex-decodes to a 16-byte key");
    const std::string plain = "the quick brown fox jumps over 0xDEADBEEF"; // arbitrary 41 bytes
    std::array<std::uint8_t, 16> iv{};                                     // zeroed IV, as the element seeds per packet
    std::string_view ivv{(char *)iv.data(), iv.size()};
    auto enc = crypto::aes_encrypt_cbc(plain, key, ivv, true);
    CHECK(enc != plain && !enc.empty(), "CBC ciphertext differs from plaintext");
    auto dec = crypto::aes_decrypt_cbc(enc, key, ivv, true);
    CHECK(dec == plain, "CBC decrypt recovers plaintext");
  }

  std::printf("\n== AES-GCM-128 roundtrip + auth (control channel path) ==\n");
  {
    const std::uint32_t seq = 7;
    std::array<std::uint8_t, 16> iv = {0};
    iv[0] = boost::endian::native_to_little(seq); // control IV scheme: seq in byte 0
    std::string_view ivv{(char *)iv.data(), iv.size()};
    const std::string plain = "INPUT_DATA payload bytes \x01\x02\x03\x04";

    auto [enc, tag] = crypto::aes_encrypt_gcm(plain, key, ivv, 16);
    CHECK(tag.size() == 16, "GCM produces a 16-byte tag");
    CHECK(enc != plain, "GCM ciphertext differs from plaintext");

    auto dec = crypto::aes_decrypt_gcm(enc, key, tag, ivv, 16);
    CHECK(dec == plain, "GCM decrypt with correct key+tag recovers plaintext");

    bool rejected = false;
    try {
      auto bad = crypto::aes_decrypt_gcm(enc, std::string(16, '\0'), tag, ivv, 16);
      rejected = (bad != plain);
    } catch (...) {
      rejected = true;
    }
    CHECK(rejected, "GCM with wrong key does not recover plaintext");
  }

  std::printf(failures == 0 ? "\nALL AES TESTS PASSED\n" : "\n%d AES TEST(S) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
