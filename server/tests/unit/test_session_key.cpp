// Session AES key/iv byte format: rikey (32 hex) -> 16-byte AES-128 key; rikeyid (decimal
// uint32) -> big-endian (iv+rtp_seq) in the first 4 bytes of a zero IV. Plus pairing key format.

#include <array>
#include <boost/endian/conversion.hpp>
#include <cstdint>
#include <cstdio>
#include <crypto/crypto.hpp>
#include <server/moonlight_proto.hpp>
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
  std::printf("== rikey/rikeyid -> element aes-key/aes-iv format ==\n");
  {
    const std::string rikey = "9d804e47a6aa6624b7d4b502b32cc522"; // 32 hex chars
    const std::string rikeyid = "16909060";                       // decimal == 0x01020304

    auto raw_key = crypto::hex_to_str(rikey, true);
    CHECK(raw_key.size() == 16, "rikey (32 hex) -> 16-byte AES-128 key");

    std::array<std::uint8_t, 16> iv{};
    std::uint32_t input_iv = std::stoul(rikeyid);
    int cur_seq = 0;
    *reinterpret_cast<std::uint32_t *>(iv.data()) = boost::endian::native_to_big(input_iv + cur_seq);
    CHECK(iv[0] == 0x01 && iv[1] == 0x02 && iv[2] == 0x03 && iv[3] == 0x04, "rikeyid -> big-endian IV first 4 bytes");
    CHECK(iv[4] == 0 && iv[15] == 0, "IV remainder zero-filled");

    int seq3 = 3;
    *reinterpret_cast<std::uint32_t *>(iv.data()) = boost::endian::native_to_big(input_iv + seq3);
    CHECK(iv[3] == 0x07, "IV low byte advances with rtp_seq (0x04 + 3 = 0x07)");
  }

  std::printf("\n== pairing AES key format (SHA256(salt+PIN)[:16]) ==\n");
  {
    auto key = moonlight::pair::gen_aes_key("a0c288cfb0ea624ec3e5cc54d6ab7e38", "7284");
    CHECK(key.size() == 16, "pairing key is 16 bytes (AES-128)");
    CHECK(crypto::str_to_hex(key) == "8A0191F59F31950D5DE3396901AA585D", "pairing key matches known answer");
  }

  std::printf(failures == 0 ? "\nALL SESSION-KEY TESTS PASSED\n" : "\n%d SESSION-KEY TEST(S) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
