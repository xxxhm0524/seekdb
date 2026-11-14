#include "storage.hpp"
#include <bit>

namespace mvd {
static constexpr size_t LEAF_MAX = (4088 - 16) / 24;
static constexpr size_t INNER_MAX = (4088 - 16) / 16;
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
        auto n = load(root);
        if (n.leaf) {
            auto it = std::lower_bound(n.keys.begin(), n.keys.end(), key);
            if (it == n.keys.end() || *it != key) return std::nullopt;
            return n.rows[it - n.keys.begin()];
        }
        size_t child = std::upper_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin();
        root = n.children.at(child);
    }
    return std::nullopt;
}
std::optional<BTree::Split> BTree::put_node(uint64_t& root, int64_t key, Ref value, bool replace) {
    if (!root) {
        root = store_.allocate_page(); owned_.insert(root);
        Node n; n.keys.push_back(key); n.rows.push_back(value); save(root, n); return std::nullopt;
    }
    auto n = load(root); root = own(root);
    if (n.leaf) {
        size_t pos = std::lower_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin();
        bool found = pos < n.keys.size() && n.keys[pos] == key;
        if (found) {
            if (!replace) fail("duplicate_key", "duplicate primary key");
            n.rows[pos] = value;
        } else {
            if (replace) throw IoError("update of missing primary key");
            n.keys.insert(n.keys.begin() + pos, key); n.rows.insert(n.rows.begin() + pos, value);
        }
        if (n.keys.size() <= LEAF_MAX) { save(root, n); return std::nullopt; }
        size_t middle = n.keys.size() / 2;
        Node right;
        right.keys.assign(n.keys.begin() + middle, n.keys.end());
        right.rows.assign(n.rows.begin() + middle, n.rows.end());
        n.keys.resize(middle); n.rows.resize(middle);
        uint64_t offset = store_.allocate_page(); owned_.insert(offset);
        save(root, n); save(offset, right); return Split{right.keys.front(), offset};
    }
    size_t pos = std::upper_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin();
    auto split = put_node(n.children[pos], key, value, replace);
    if (split) {
        n.keys.insert(n.keys.begin() + pos, split->key);
        n.children.insert(n.children.begin() + pos + 1, split->right);
    }
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
