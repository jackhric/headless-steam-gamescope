// FEC (nanors Reed-Solomon) encode -> erase -> decode roundtrip. Contiguous shard layout
// matches the payloader: shards[i] = buf + i*block_size, data shards first then parity.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <moonlight/fec.hpp>
#include <vector>

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

// parity = ceil(data*fec/100), floored at min_required.
static int parity_for(int data_shards, int fec_percentage, int min_required) {
  int parity = (data_shards * fec_percentage + 99) / 100;
  if (parity < min_required)
    parity = min_required;
  return parity;
}

static bool roundtrip(int data_shards, int parity_shards, int block_size, int n_erasures) {
  const int nr = data_shards + parity_shards;
  std::vector<std::uint8_t> buf((std::size_t)nr * block_size, 0);
  for (int i = 0; i < data_shards * block_size; i++)
    buf[i] = (std::uint8_t)((i * 131 + 17) & 0xff);
  const std::vector<std::uint8_t> original(buf.begin(), buf.begin() + (std::size_t)data_shards * block_size);

  std::vector<std::uint8_t *> ptr(nr);
  for (int i = 0; i < nr; i++)
    ptr[i] = buf.data() + (std::size_t)i * block_size;

  auto rs = moonlight::fec::create(data_shards, parity_shards);
  if (!rs)
    return false;
  if (moonlight::fec::encode(rs.get(), ptr.data(), nr, block_size) != 0)
    return false;

  std::vector<std::uint8_t> marks(nr, 0);
  for (int i = 0; i < n_erasures; i++) {
    marks[i] = 1;
    std::memset(ptr[i], 0, block_size);
  }
  if (moonlight::fec::decode(rs.get(), ptr.data(), marks.data(), nr, block_size) != 0)
    return false;

  return std::memcmp(buf.data(), original.data(), (std::size_t)data_shards * block_size) == 0;
}

int main() {
  moonlight::fec::init();
  std::printf("== FEC encode -> erase -> decode roundtrip ==\n");

  {
    int d = 10, p = parity_for(d, 20, 2);
    CHECK(p == 2, "video 10 data @20% -> 2 parity shards");
    CHECK(roundtrip(d, p, 1024, 0), "10+2 no loss decodes clean");
    CHECK(roundtrip(d, p, 1024, 1), "10+2 recovers 1 erased data shard");
    CHECK(roundtrip(d, p, 1024, 2), "10+2 recovers 2 erased data shards (== parity)");
  }

  // Audio FEC fires every 4 packets.
  {
    int d = 4, p = parity_for(d, 20, 2);
    CHECK(p == 2, "audio 4 data @20% floored to 2 parity shards");
    CHECK(roundtrip(d, p, 256, 2), "4+2 recovers 2 erased data shards");
  }

  {
    int d = 30, p = parity_for(d, 33, 2);
    CHECK(roundtrip(d, p, 512, p), "30 data recovers a full parity-count burst loss");
  }

  std::printf(failures == 0 ? "\nALL FEC TESTS PASSED\n" : "\n%d FEC TEST(S) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
