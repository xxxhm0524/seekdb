#pragma once
#include "model.hpp"
#include <array>
#include <filesystem>
#include <functional>
#include <list>
#include <unordered_map>
#include <unordered_set>

namespace mvd {
constexpr size_t PAGE_SIZE = 4096;
using Page = std::array<unsigned char, PAGE_SIZE>;
uint64_t checksum(const void* data, size_t size);
uint64_t read_le(const unsigned char* data, unsigned bytes);
void write_le(unsigned char* data, uint64_t value, unsigned bytes);

class File {
    int fd_ = -1;
    std::filesystem::path path_;
public:
    explicit File(const std::filesystem::path& path);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    void read(uint64_t offset, void* data, size_t size) const;
    void write(uint64_t offset, const void* data, size_t size);
    uint64_t size() const;
    void truncate(uint64_t size);
    void sync();
    void replace_with(const std::filesystem::path& replacement);
};

// Committed pages are immutable. Only pages owned by the current SQL statement
// can be edited; errors therefore discard a tail, never overwrite live data.
class Storage {
    std::filesystem::path directory_;
    File file_;
    struct Cached { Page page; bool dirty; std::list<uint64_t>::iterator lru; };
    std::unordered_map<uint64_t, Cached> pages_;
    std::list<uint64_t> lru_;
    size_t cache_pages_;
    uint64_t end_ = PAGE_SIZE * 2, durable_end_ = PAGE_SIZE * 2, generation_ = 0;
    void evict();
    void flush_page(uint64_t offset, Cached& page);
    void cleanup_compaction_scratch();
public:
    Catalog committed;
    explicit Storage(const std::filesystem::path& directory);
    Page page(uint64_t offset);
    void set_page(uint64_t offset, const Page& page);
    uint64_t allocate_page();
    Ref append(const std::string& bytes);
    std::string read(Ref ref) const;
    Row read_selected(const Table& table, Ref ref, const std::vector<bool>& wanted) const;
    uint64_t mark() const { return end_; }
    void rewind(uint64_t mark);
    void rollback() { rewind(durable_end_); }
    void commit(const Catalog& catalog);
    bool compact_if_needed();
};

class BTree {
    Storage& store_;
    std::unordered_set<uint64_t> owned_;
    struct Node {
        bool leaf = true;
        std::vector<int64_t> keys;
        std::vector<Ref> rows;
        std::vector<uint64_t> children;
    };
    struct Split { int64_t key; uint64_t right; };
    Node load(uint64_t offset);
    void save(uint64_t offset, const Node& node);
    uint64_t own(uint64_t offset);
    std::optional<Split> put_node(uint64_t& root, int64_t key, Ref value, bool replace);
    bool erase_node(uint64_t& root, int64_t key);
public:
    explicit BTree(Storage& store) : store_(store) {}
    std::optional<Ref> find(uint64_t root, int64_t key);
    void put(uint64_t& root, int64_t key, Ref value, bool replace = false);
    bool erase(uint64_t& root, int64_t key);
    void scan(uint64_t root, const std::function<bool(int64_t, Ref)>& visit);
};
}
