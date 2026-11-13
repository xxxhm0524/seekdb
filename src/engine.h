#pragma once
#include "model/result.h"
#include "core/error.h"
#include <memory>
namespace mvd { namespace detail { class Database; }
class Engine {
 public:
  Engine(std::string data_dir,size_t cache_pages=1024,size_t tx_memory=8*1024*1024,
         size_t ann_min_rows=40000,size_t sort_memory=16*1024*1024);
  ~Engine();
  Engine(const Engine&)=delete;
  Engine& operator=(const Engine&)=delete;
  void open();
  void shutdown();
  Response execute(const std::string& sql);
  bool ann_mode() const;
  void set_ann_mode(bool value);
  bool in_transaction() const;
 private:
  std::unique_ptr<detail::Database> database_;
};
}
