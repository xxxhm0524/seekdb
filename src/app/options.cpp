#include "options.h"
#include <cstdlib>
#include <cstdio>
namespace mvd::app {
Options parse_options(int argc, char **argv) {
  Options opt;
  if (const char *env = std::getenv("VECTORDB_DATA_DIR")) opt.data_dir = env;
  else if (const char *env = std::getenv("MVD_DATA_DIR")) opt.data_dir = env;
  else if (const char *env = std::getenv("MVDB_DATA_DIR")) opt.data_dir = env;
  if (const char *env = std::getenv("MVD_QUERY_THREADS")) opt.query_threads = std::atoi(env);
  if (const char *env = std::getenv("MVD_CACHE_MB")) opt.cache_mb = std::atoi(env);
  if (const char *env = std::getenv("MVD_TX_MEMORY_MB")) opt.tx_memory_mb = std::atoi(env);
  if (const char *env = std::getenv("MVD_ANN_MIN_ROWS")) opt.ann_min_rows = std::atoi(env);
  if (const char *env = std::getenv("MVD_SORT_MEMORY_MB")) opt.sort_memory_mb = std::atoi(env);
  if (const char *env = std::getenv("MVD_VECTOR_MODE")) {
    std::string m = env;
    opt.ann_default = (m == "ann" || m == "approximate");
  }
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto take = [&](const std::string &prefix, std::string &value) -> bool {
      if (arg.rfind(prefix, 0) == 0) {
        value = arg.substr(prefix.size());
        return true;
      }
      return false;
    };
    std::string v;
    if (take("--data-dir=", v)) opt.data_dir = v;
    else if (arg == "--data-dir" && i + 1 < argc) opt.data_dir = argv[++i];
    else if (take("--query-threads=", v)) opt.query_threads = std::atoi(v.c_str());
    else if (arg == "--query-threads" && i + 1 < argc) opt.query_threads = std::atoi(argv[++i]);
    else if (take("--cache-mb=", v)) opt.cache_mb = std::atoi(v.c_str());
    else if (arg == "--cache-mb" && i + 1 < argc) opt.cache_mb = std::atoi(argv[++i]);
    else if (take("--tx-memory-mb=", v)) opt.tx_memory_mb = std::atoi(v.c_str());
    else if (arg == "--tx-memory-mb" && i + 1 < argc) opt.tx_memory_mb = std::atoi(argv[++i]);
    else if (take("--sort-memory-mb=", v)) opt.sort_memory_mb = std::atoi(v.c_str());
    else if (arg == "--sort-memory-mb" && i + 1 < argc) opt.sort_memory_mb = std::atoi(argv[++i]);
    else if (take("--ann-min-rows=", v)) opt.ann_min_rows = std::atoi(v.c_str());
    else if (arg == "--ann-min-rows" && i + 1 < argc) opt.ann_min_rows = std::atoi(argv[++i]);
    else if (take("--vector-mode=", v)) opt.ann_default = (v == "ann" || v == "approximate");
    else if (arg == "--vector-mode" && i + 1 < argc) {
      opt.ann_default = (std::string(argv[i + 1]) == "ann" ||
                         std::string(argv[i + 1]) == "approximate");
      ++i;
    } else if (take("--mode=", v)) {
      opt.ann_default = (v == "ann" || v == "approximate");
    } else if (arg == "--mode" && i + 1 < argc) {
      opt.ann_default = (std::string(argv[i + 1]) == "ann" ||
                         std::string(argv[i + 1]) == "approximate");
      ++i;
    } else if (arg == "--ann") {
      opt.ann_default = true;
    } else if (arg == "--exact") {
      opt.ann_default = false;
    } else if (arg == "--help" || arg == "-h") {
      std::fprintf(stderr,
                   "minivectordb [--data-dir=/data] [--query-threads=N] "
                   "[--cache-mb=N] [--tx-memory-mb=N] [--sort-memory-mb=N] [--ann-min-rows=N] [--vector-mode=exact|ann]\n");
      std::exit(0);
    }
  }
  // Older output-format flags are accepted but ignored: stdout is always the
  // JSONL judge protocol.
  if (opt.query_threads < 1) opt.query_threads = 1;
  if (opt.cache_mb < 1) opt.cache_mb = 1;
  if (opt.tx_memory_mb < 1) opt.tx_memory_mb = 1;
  if (opt.ann_min_rows < 1) opt.ann_min_rows = 1;
  if (opt.sort_memory_mb < 1) opt.sort_memory_mb = 1;
  return opt;
}

}
