#include "engine.hpp"
#include <set>
#include <numeric>
namespace mvd {
static int compare_value(const Value& a, const Value& b) {
    if (a.index() != b.index()) fail("type_error", "incompatible comparison operands");
    if (auto x = std::get_if<int64_t>(&a)) { auto y = std::get<int64_t>(b); return (*x > y) - (*x < y); }
    if (auto x = std::get_if<std::string>(&a)) { const auto& y = std::get<std::string>(b); return (*x > y) - (*x < y); }
    fail("unsupported", "vector comparisons require L2_DISTANCE");
}
std::vector<Predicate> bind_predicates(const Table& t, const std::vector<Condition>& conditions) {
    std::vector<Predicate> out;
    for (const auto& c : conditions) {
        size_t i = t.column(c.column);
        if (t.columns[i].type == Type::Vector) fail("unsupported", "WHERE requires an ordinary column");
        // A comparison string need not fit the stored VARCHAR width.
        Column type = t.columns[i]; if (type.type == Type::Text) type.width = UINT32_MAX;
        out.push_back({i, c.op, parse_value(type, c.value.text, c.value.quoted)});
    }
    return out;
}
bool matches(const Row& row, const std::vector<Predicate>& predicates) {
    for (const auto& p : predicates) {
        int c = compare_value(row[p.column], p.value);
        if ((p.op == "=" && c != 0) || (p.op == "<" && c >= 0) || (p.op == "<=" && c > 0) ||
            (p.op == ">" && c <= 0) || (p.op == ">=" && c < 0) || ((p.op == "!=" || p.op == "<>") && c == 0)) return false;
    }
    return true;
}
Engine::Engine(const std::filesystem::path& dir) : directory_(dir), storage_(dir), active_(storage_.committed) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        bool sort_temp = name.starts_with("sort-") && name.ends_with(".tmp") && name.size() > 9;
        if (sort_temp) for (size_t i = 5; i + 4 < name.size(); ++i) sort_temp &= name[i] >= '0' && name[i] <= '9';
        if (name == "response.tmp" || sort_temp) std::filesystem::remove(entry.path());
    }
    // All rebuild work that can fit the bounded index budget completes before
    // the protocol's ready handshake. Oversized tables use disk exact scans.
    for (const auto& table : active_) if (!table.indexes.empty()) {
        vector_index_.reset();
        vector_index_ = VectorIndex::create(storage_, table);
    }
}
size_t Engine::table_index(const std::string& name) const {
    for (size_t i = 0; i < active_.size(); ++i) if (active_[i].name == lower(name)) return i;
    fail("unknown_table", "unknown table: " + name);
}
void Engine::shutdown() {
    vector_index_.reset(); refused_vector_root_.reset();
    if (transaction_) {
        Catalog next = storage_.committed;
        storage_.rollback(); active_.swap(next); transaction_ = false;
    }
}
Result Engine::execute(const std::string& sql, const std::string& mode) {
    if (mode != "exact" && mode != "ann") fail("invalid_request", "unknown search_mode");
    Statement s = parse_sql(sql);
    if (s.kind == Statement::Select) return select(s);
    vector_index_.reset(); refused_vector_root_.reset();
    if (!transaction_ && storage_.compact_if_needed()) active_ = storage_.committed;
    if (s.kind == Statement::Begin) {
        if (transaction_) fail("transaction_error", "nested transactions are unsupported");
        transaction_ = true; return {};
    }
    if (s.kind == Statement::Commit) {
        if (!transaction_) fail("transaction_error", "no active transaction");
        storage_.commit(active_); transaction_ = false; return {};
    }
    if (s.kind == Statement::Rollback) {
        if (!transaction_) fail("transaction_error", "no active transaction");
        shutdown(); return {};
    }
    Catalog next = active_;
    uint64_t mark = storage_.mark();
    Result result;
    std::unique_ptr<VectorIndex> built_index;
    try {
        BTree tree(storage_);
        if (s.kind == Statement::Create) {
            for (const auto& t : next) if (t.name == lower(s.table)) fail("table_exists", "table already exists");
            next.push_back(std::move(s.definition));
        } else {
            size_t ti = table_index(s.table); auto& t = next[ti];
            if (s.kind == Statement::Index) {
                size_t col = t.column(s.insert_columns.at(0));
                if (t.columns[col].type != Type::Vector) fail("type_error", "vector index requires VECTOR column");
                for (const auto& name : t.indexes) if (lower(name) == lower(s.index)) fail("index_exists", "index already exists");
                t.indexes.push_back(s.index);
                built_index = VectorIndex::create(storage_, t);
            } else if (s.kind == Statement::Insert) {
                std::vector<size_t> order;
                if (s.insert_columns.empty()) { order.resize(t.columns.size()); std::iota(order.begin(), order.end(), 0); }
                else for (const auto& name : s.insert_columns) order.push_back(t.column(name));
                std::set<size_t> distinct(order.begin(), order.end());
                if (order.size() != t.columns.size() || distinct.size() != order.size()) fail("column_count", "INSERT must specify each column exactly once");
                // Batch only record bytes. Page ownership, statement rollback,
                // commit barriers and acknowledgement ordering remain unchanged.
                std::string batch;
                struct Pending { int64_t key; uint32_t offset, length; };
                std::vector<Pending> pending;
                auto flush = [&] {
                    if (pending.empty()) return;
                    const Ref data = storage_.append(batch);
                    for (const auto& item : pending) {
                        tree.put(t.root, item.key, {data.offset + item.offset, item.length});
                        ++t.count; t.live_bytes += item.length;
                    }
                    batch.clear(); pending.clear();
                };
                for (const auto& literals : s.values) {
                    if (literals.size() != order.size()) fail("column_count", "incorrect INSERT value count");
                    Row row(t.columns.size());
                    for (size_t i = 0; i < order.size(); ++i) row[order[i]] = parse_value(t.columns[order[i]], literals[i].text, literals[i].quoted);
                    int64_t key = std::get<int64_t>(row[t.primary]);
                    std::string bytes = encode_row(t, row);
                    if (bytes.size() > 65536) {
                        flush(); Ref ref = storage_.append(bytes); tree.put(t.root, key, ref);
                        ++t.count; t.live_bytes += ref.length;
                    } else {
                        if (bytes.size() > 65536 - batch.size() || pending.size() >= 1024) flush();
                        pending.push_back({key, uint32_t(batch.size()), uint32_t(bytes.size())});
                        batch += bytes;
                    }
                }
                flush();
                result.fields["affected_rows"] = s.values.size();
            } else {
                auto predicates = bind_predicates(t, s.conditions);
                std::vector<std::pair<size_t, Value>> assignments;
                std::set<size_t> assigned;
                for (const auto& [name, literal] : s.assignments) {
                    size_t col = t.column(name);
                    if (col == t.primary) fail("unsupported", "primary key updates are unsupported");
                    if (!assigned.insert(col).second) fail("syntax_error", "duplicate UPDATE assignment");
                    assignments.emplace_back(col, parse_value(t.columns[col], literal.text, literal.quoted));
                }
                uint64_t changed = 0;
                const Table snapshot = active_[ti];
                std::vector<bool> needed(snapshot.columns.size(), false);
                for (const auto& p : predicates) needed[p.column] = true;
                needed[snapshot.primary] = false;
                bool predicate_payload = std::any_of(needed.begin(), needed.end(), [](bool x) { return x; });
                auto visit = [&](int64_t key, Ref ref) {
                    Row row;
                    if (s.kind != Statement::Delete) row = decode_row(snapshot, storage_.read(ref));
                    else if (predicate_payload) row = storage_.read_selected(snapshot, ref, needed);
                    else row.resize(snapshot.columns.size());
                    row[snapshot.primary] = key;
                    if (!matches(row, predicates)) return true;
                    if (s.kind == Statement::Delete) {
                        if (!tree.erase(t.root, key)) throw IoError("missing row during DELETE");
                        --t.count;
                        t.live_bytes -= ref.length;
                    } else {
                        for (const auto& [col, value] : assignments) row[col] = value;
                        Ref replacement = storage_.append(encode_row(t, row));
                        tree.put(t.root, key, replacement, true);
                        t.live_bytes = t.live_bytes - ref.length + replacement.length;
                    }
                    ++changed; return true;
                };
                std::optional<int64_t> point;
                for (const auto& p : predicates) if (p.column == t.primary && p.op == "=") point = std::get<int64_t>(p.value);
                if (point) { if (auto ref = tree.find(snapshot.root, *point)) visit(*point, *ref); }
                else tree.scan(snapshot.root, visit);
                result.fields["affected_rows"] = changed;
            }
        }
        if (!transaction_) storage_.commit(next);
        active_.swap(next);
        vector_index_ = std::move(built_index);
    } catch (const IoError&) { throw; }
    catch (...) { storage_.rewind(mark); throw; }
    return result;
}
}
