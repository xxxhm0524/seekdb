#include "layout.h"
#include <mutex>
#include <vector>
namespace mvd {
namespace {
class PagePool {
 public:
  PagePool() { spare_.reserve(kIoBatchPages); }
  ~PagePool() { for (Page *page : spare_) delete page; }
  Page *take() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (spare_.empty()) return new Page();
    Page *page = spare_.back();
    spare_.pop_back();
    return page;
  }
  void put(Page *page) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Retain a complete I/O batch. A smaller pool frees aligned pages between
    // small cache entries on every scan, fragmenting the allocator's heap.
    if (spare_.size() < kIoBatchPages) spare_.push_back(page);
    else delete page;
  }
 private:
  std::mutex mutex_;
  std::vector<Page *> spare_;
};
PagePool &page_pool() {
  static PagePool pool;
  return pool;
}
}  // namespace

std::shared_ptr<Page> make_page(const Page *source) {
  Page *page = page_pool().take();
  if (source) *page = *source;
  else *page = Page();
  return std::shared_ptr<Page>(page, [](Page *released) { page_pool().put(released); });
}

// ---------------------------------------------------------------------------
// Schema layout
// ---------------------------------------------------------------------------
}
