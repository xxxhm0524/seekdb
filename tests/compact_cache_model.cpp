#include "compact_cache.h"
#include "tx_spool.h"
#include <cassert>
#include <iostream>
#include <random>

int main(int argc, char **argv) {
  assert(argc == 2);
  std::mt19937_64 random(20261124);
  std::array<uint8_t, mvd::kCompactPageBytes> block{}, output{};
  mvd::CompactBlockCache cache(4096);
  for (uint64_t key = 0; key < 128; ++key) {
    std::memset(block.data(), static_cast<int>(key), block.size());
    std::memcpy(block.data(), &key, sizeof(key));
    cache.put(key, block.data());
    assert(cache.memory_bytes() <= 4096);
    if (cache.contains(key)) assert(cache.get(key, output.data()) && output == block);
  }
  assert(cache.entries() < 128 && cache.get(0, output.data()) && !cache.get(127, output.data()));
  for (auto &byte : block) byte = static_cast<uint8_t>(random());
  cache.put(0, block.data());
  assert(!cache.get(0, output.data())); // Rejected replacement invalidates old contents.
  cache.erase_if([](uint64_t) { return true; });
  assert(cache.memory_bytes() == 0);
  mvd::CompactBlockCache disabled(0);
  disabled.put(1, block.data());
  assert(!disabled.get(1, output.data()) && disabled.memory_bytes() == 0);

  mvd::TxSpool spool(argv[1]);
  std::vector<uint8_t> oracle, row, actual;
  auto append = [&](int count, int salt) {
    for (int i = 0; i < count; ++i) {
      size_t n = i%3 == 0 ? 8256 : i%3 == 1 ? 93 : 520;
      row.assign(n, static_cast<uint8_t>((i+salt)%251));
      uint64_t marker = random();
      std::memcpy(row.data(), &marker, sizeof(marker));
      uint64_t offset = spool.append(row);
      assert(offset == oracle.size());
      oracle.insert(oracle.end(), row.begin(), row.end());
    }
  };
  auto verify = [&]() {
    for (int i = 0; i < 4000; ++i) {
      size_t offset = static_cast<size_t>(random()%oracle.size());
      uint32_t length = static_cast<uint32_t>(std::min<size_t>(random()%10000+1, oracle.size()-offset));
      spool.read(offset, length, actual);
      assert(std::equal(actual.begin(), actual.end(), oracle.begin()+offset));
    }
    assert(spool.size() == oracle.size());
  };
  append(1200, 1); verify();
  uint64_t mark = spool.size();
  append(400, 2); verify();
  spool.truncate(mark); oracle.resize(mark);
  append(500, 3); verify();
  spool.truncate(65536); oracle.resize(65536);
  append(50, 7); verify();
  spool.truncate(0); oracle.clear();
  append(100, 17); verify();
  bool rejected = false;
  try { spool.read(spool.size(), 1, actual); } catch (const mvd::DbError &) { rejected = true; }
  assert(rejected);
  std::cout << "{\"passed\":true,\"random_spool_reads\":20000,\"rewind_and_offset_reuse\":true,\"bounded_admission_and_replacement\":true}\n";
}
