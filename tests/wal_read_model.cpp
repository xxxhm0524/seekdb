#if __has_include("../src/storage/store.h")
#include "../src/storage/store.h"
#endif
#include "engine.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
static uint64_t read_calls() {
#ifdef _WIN32
  IO_COUNTERS value{};
  require(GetProcessIoCounters(GetCurrentProcess(), &value), "cannot read I/O counters");
  return value.ReadOperationCount;
#else
  std::ifstream input("/proc/self/io");
  std::string key; uint64_t value;
  while (input >> key >> value) if (key == "syscr:") return value;
  throw std::runtime_error("syscr is missing");
#endif
}
static void fill(mvd::Page &page, uint32_t serial) {
  std::memset(static_cast<void *>(&page), static_cast<int>(serial % 251 + 1), sizeof(page));
  page.magic = mvd::kPageMagic; page.nrows = 1; page.page_no = serial;
}
static void same(const mvd::Page &page, uint32_t serial) {
  auto expected = std::make_unique<mvd::Page>(); fill(*expected, serial);
  require(std::memcmp(&page, expected.get(), sizeof(page)) == 0, "WAL image bytes differ");
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "supply a temporary WAL path");
    mvd::Wal wal; require(wal.open(argv[1]), "open failed");
    auto page = std::make_unique<mvd::Page>();
    std::vector<uint64_t> offsets;
    auto txid = wal.allocate_txid();
    for (uint32_t i = 0; i < 147; ++i) {
      fill(*page, i); offsets.push_back(static_cast<uint64_t>(wal.size()));
      require(wal.append_page(txid, 7, i, *page), "append failed");
    }
    require(wal.append_commit(txid) && wal.sync(), "sync failed");
    auto before = read_calls();
    for (uint32_t i = 0; i < offsets.size(); ++i) same(*wal.read_page_image(offsets[i]), i);
    uint64_t reads = read_calls() - before;
    require(reads < 16, "WAL image reads were not batched: "+std::to_string(reads));
    for (size_t i = offsets.size(); i-- > 0;) same(*wal.read_page_image(offsets[i]), static_cast<uint32_t>(i));
    size_t recovered = 0;
    wal.recover([&](uint32_t table, uint32_t number, std::shared_ptr<mvd::Page> value) {
      require(table == 7, "wrong table"); same(*value, number); ++recovered;
    });
    require(recovered == offsets.size(), "recovery missed committed pages");
    fill(*page, 999);
    require(wal.append_page(wal.allocate_txid(), 7, 999, *page) && wal.sync(), "uncommitted append failed");
    recovered = 0;
    wal.recover([&](uint32_t, uint32_t number, std::shared_ptr<mvd::Page> value) {
      require(number < 147, "uncommitted image was recovered"); same(*value, number); ++recovered;
    });
    require(recovered == offsets.size(), "append invalidated earlier committed pages");
    // Prime exactly the offsets about to be reused, without closing the WAL.
    same(*wal.read_page_image(offsets[0]), 0);
    require(wal.truncate(), "truncate failed");
    fill(*page, 222); txid = wal.allocate_txid();
    require(wal.append_page(txid, 7, 222, *page) && wal.append_commit(txid) && wal.sync(), "reuse append failed");
    same(*wal.read_page_image(0), 222);
    wal.close();
    bool closed_failed = false;
    try { (void)wal.read_page_image(0); } catch (const mvd::DbError &) { closed_failed = true; }
    require(closed_failed, "closed WAL served cached bytes");
    {
      mvd::Wal writer; require(writer.open(argv[1]) && writer.truncate(), "replacement open failed");
      fill(*page, 223); txid = writer.allocate_txid();
      require(writer.append_page(txid, 7, 223, *page) && writer.append_commit(txid) && writer.sync(), "replacement failed");
    }
    require(wal.open(argv[1]), "reopen failed"); same(*wal.read_page_image(0), 223);
    auto cutoff = static_cast<uint64_t>(wal.size()) - sizeof(mvd::WalRecordHeader) - 17;
    wal.close();
    {
      auto file = mvd::FileHandle::open(argv[1], O_RDWR);
      require(file.ftruncate(cutoff) && file.fsync(), "tail truncation failed");
    }
    require(wal.open(argv[1]), "partial reopen failed");
    bool tail_failed = false;
    try { (void)wal.read_page_image(0); } catch (const mvd::DbError &) { tail_failed = true; }
    require(tail_failed, "incomplete image was returned");
    recovered = 0;
    wal.recover([&](uint32_t, uint32_t, std::shared_ptr<mvd::Page>) { ++recovered; });
    require(recovered == 0, "incomplete tail was recovered");
    wal.close();
    std::cout << "{\"passed\":true,\"sequential_pages\":147,\"read_operations\":" << reads
              << ",\"truncate_append_reopen\":true,\"uncommitted_and_partial_tail\":true}" << std::endl;
    return 0;
  } catch (const mvd::DbError &error) {
    std::cerr << error.code << ": " << error.message << std::endl;
    return 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
