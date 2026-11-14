#include "storage.hpp"
#include <bit>
#include <cstring>

namespace mvd {
static constexpr size_t LEAF_MAX = (4088 - 16) / 24;
static constexpr size_t INNER_MAX = (4088 - 16) / 16;
static size_t page_count(const Page& p) {
    if (p[0] != 1 && p[0] != 2) throw IoError("invalid B+ tree page kind");
    const size_t count = read_le(p.data() + 2, 2);
    if (count > (p[0] == 1 ? LEAF_MAX : INNER_MAX)) throw IoError("invalid B+ tree page count");
    return count;
}
static size_t page_bound(const Page& p, size_t count, int64_t key, bool upper) {
    size_t begin = 0, end = count; const size_t stride = p[0] == 1 ? 24 : 16;
    while (begin < end) {
        const size_t middle = begin + (end - begin) / 2;
        const int64_t value = std::bit_cast<int64_t>(read_le(p.data() + 16 + middle * stride, 8));
        if (value < key || (upper && value == key)) begin = middle + 1; else end = middle;
    }
    return begin;
}
static size_t child_position(size_t index) { return index ? 24 + (index - 1) * 16 : 8; }
BTree::Node BTree::load(uint64_t offset) {
    auto p = store_.page(offset);
    if (p[0] != 1 && p[0] != 2) throw IoError("invalid B+ tree page kind");
    Node n; n.leaf = p[0] == 1;
    size_t count = read_le(p.data() + 2, 2);
    if (count > (n.leaf ? LEAF_MAX : INNER_MAX)) throw IoError("invalid B+ tree page count");
    n.keys.reserve(count);
    if (!n.leaf) n.children.push_back(read_le(p.data() + 8, 8));
    for (size_t i = 0; i < count; ++i) {
        const auto* e = p.data() + 16 + i * (n.leaf ? 24 : 16);
        n.keys.push_back(std::bit_cast<int64_t>(read_le(e, 8)));
        if (n.leaf) n.rows.push_back({read_le(e + 8, 8), static_cast<uint32_t>(read_le(e + 16, 4))});
        else n.children.push_back(read_le(e + 8, 8));
    }
    return n;
}
void BTree::save(uint64_t offset, const Node& n) {
    if (n.keys.size() > (n.leaf ? LEAF_MAX : INNER_MAX)) throw IoError("B+ tree page overflow");
    Page p{}; p[0] = n.leaf ? 1 : 2; write_le(p.data() + 2, n.keys.size(), 2);
    if (!n.leaf) write_le(p.data() + 8, n.children.at(0), 8);
    for (size_t i = 0; i < n.keys.size(); ++i) {
        auto* e = p.data() + 16 + i * (n.leaf ? 24 : 16);
        write_le(e, std::bit_cast<uint64_t>(n.keys[i]), 8);
        if (n.leaf) { write_le(e + 8, n.rows[i].offset, 8); write_le(e + 16, n.rows[i].length, 4); }
        else write_le(e + 8, n.children.at(i + 1), 8);
    }
    store_.set_page(offset, p);
}
uint64_t BTree::own(uint64_t offset) {
    if (owned_.contains(offset)) return offset;
    Page p = store_.page(offset);
    uint64_t next = store_.allocate_page();
    owned_.insert(next); store_.set_page(next, p); return next;
}
std::optional<Ref> BTree::find(uint64_t root, int64_t key) {
    while (root) {
        const auto p = store_.page(root); const size_t count = page_count(p);
        const size_t pos = page_bound(p, count, key, p[0] != 1);
        if (p[0] == 1) {
            if (pos == count) return {};
            const auto* entry = p.data() + 16 + pos * 24;
            if (std::bit_cast<int64_t>(read_le(entry, 8)) != key) return {};
            return Ref{read_le(entry + 8, 8), uint32_t(read_le(entry + 16, 4))};
        }
        root = read_le(p.data() + child_position(pos), 8);
    }
    return std::nullopt;
}
std::optional<BTree::Split> BTree::put_node(uint64_t& root, int64_t key, Ref value, bool replace) {
    if (!root) {
        root = store_.allocate_page(); owned_.insert(root);
        Node n; n.keys.push_back(key); n.rows.push_back(value); save(root, n); return std::nullopt;
    }
    auto p = store_.page(root); const size_t count = page_count(p);
    if (p[0] == 1) {
        const size_t pos = page_bound(p, count, key, false);
        const bool found = pos < count && std::bit_cast<int64_t>(read_le(p.data() + 16 + pos * 24, 8)) == key;
        if (found && !replace) fail("duplicate_key", "duplicate primary key");
        if (!found && replace) throw IoError("update of missing primary key");
        root = own(root);
        if (found || count < LEAF_MAX) {
            auto* entry = p.data() + 16 + pos * 24;
            if (!found) {
                std::memmove(entry + 24, entry, (count - pos) * 24);
                write_le(p.data() + 2, count + 1, 2);
            }
            write_le(entry, std::bit_cast<uint64_t>(key), 8); write_le(entry + 8, value.offset, 8);
            write_le(entry + 16, value.length, 4); store_.set_page(root, p); return {};
        }
        auto n = load(root);
        n.keys.insert(n.keys.begin() + pos, key); n.rows.insert(n.rows.begin() + pos, value);
        size_t middle = n.keys.size() / 2;
        Node right;
        right.keys.assign(n.keys.begin() + middle, n.keys.end());
        right.rows.assign(n.rows.begin() + middle, n.rows.end());
        n.keys.resize(middle); n.rows.resize(middle);
        uint64_t offset = store_.allocate_page(); owned_.insert(offset);
        save(root, n); save(offset, right); return Split{right.keys.front(), offset};
    }
    const size_t pos = page_bound(p, count, key, true);
    const size_t pointer = child_position(pos); const uint64_t previous = read_le(p.data() + pointer, 8);
    uint64_t child = previous; auto split = put_node(child, key, value, replace);
    // An already-owned child may change without changing its address. Its
    // parent's immutable separator bytes then need neither decoding nor saving.
    if (!split && child == previous) return {};
    root = own(root);
    if (!split) { write_le(p.data() + pointer, child, 8); store_.set_page(root, p); return {}; }
    auto n = load(root); n.children[pos] = child;
    n.keys.insert(n.keys.begin() + pos, split->key);
    n.children.insert(n.children.begin() + pos + 1, split->right);
    if (n.keys.size() <= INNER_MAX) { save(root, n); return std::nullopt; }
    size_t middle = n.keys.size() / 2;
    int64_t separator = n.keys[middle];
    Node right; right.leaf = false;
    right.keys.assign(n.keys.begin() + middle + 1, n.keys.end());
    right.children.assign(n.children.begin() + middle + 1, n.children.end());
    n.keys.resize(middle); n.children.resize(middle + 1);
    uint64_t offset = store_.allocate_page(); owned_.insert(offset);
    save(root, n); save(offset, right); return Split{separator, offset};
}
void BTree::put(uint64_t& root, int64_t key, Ref value, bool replace) {
    auto split = put_node(root, key, value, replace);
    if (split) {
        Node n; n.leaf = false; n.keys.push_back(split->key); n.children = {root, split->right};
        root = store_.allocate_page(); owned_.insert(root); save(root, n);
    }
}
bool BTree::erase_node(uint64_t& root, int64_t key) {
    if (!root) return false;
    auto n = load(root);
    if (n.leaf) {
        size_t pos = std::lower_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin();
        if (pos == n.keys.size() || n.keys[pos] != key) return false;
        root = own(root); n.keys.erase(n.keys.begin() + pos); n.rows.erase(n.rows.begin() + pos); save(root, n); return true;
    }
    size_t pos = std::upper_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin();
    if (!erase_node(n.children[pos], key)) return false;
    root = own(root); save(root, n); return true;
}
bool BTree::erase(uint64_t& root, int64_t key) { return erase_node(root, key); }
void BTree::scan(uint64_t root, const std::function<bool(int64_t, Ref)>& visit) {
    std::function<bool(uint64_t)> walk = [&](uint64_t offset) {
        if (!offset) return true;
        auto n = load(offset);
        if (n.leaf) {
            for (size_t i = 0; i < n.keys.size(); ++i) if (!visit(n.keys[i], n.rows[i])) return false;
        } else for (uint64_t child : n.children) if (!walk(child)) return false;
        return true;
    };
    walk(root);
}
}
