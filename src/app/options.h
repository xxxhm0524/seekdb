#pragma once
#include <string>
namespace mvd::app {
struct Options {
  std::string data_dir = "/data";
  int query_threads = 2;
  int cache_mb = 64;
  int tx_memory_mb = 8;
  int ann_min_rows = 40000;
  int sort_memory_mb = 16;
  bool ann_default = false;
};

Options parse_options(int,char**);
}
