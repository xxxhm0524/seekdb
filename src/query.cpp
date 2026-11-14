#include "engine.hpp"
#include <bit>
#include <fstream>
#include <queue>

namespace mvd {
namespace {
struct BoundExpr {
    size_t column;
    std::vector<float> query;
    bool distance = false;
    std::string name;
};
BoundExpr bind_expr(const Table& table, const Expr& expr) {
    BoundExpr b{table.column(expr.column), {}, bool(expr.vector), expr.alias};
    if (expr.vector) {
        if (table.columns[b.column].type != Type::Vector) fail("type_error", "L2_DISTANCE requires a VECTOR column");
        b.query = std::get<std::vector<float>>(parse_value(table.columns[b.column], expr.vector->text, expr.vector->quoted));
        if (b.name.empty()) b.name = "l2_distance";
    } else if (b.name.empty()) b.name = table.columns[b.column].name;
    return b;
}
double distance(const std::vector<float>& a, const std::vector<float>& b) {
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) { double d = double(a[i]) - double(b[i]); sum += d * d; }
    return std::sqrt(sum);
}
using Atom = std::variant<int64_t, std::string, double>;
Atom evaluate(const Row& row, const BoundExpr& expr) {
    if (expr.distance) return distance(std::get<std::vector<float>>(row[expr.column]), expr.query);
    if (auto i = std::get_if<int64_t>(&row[expr.column])) return *i;
    if (auto s = std::get_if<std::string>(&row[expr.column])) return *s;
    fail("unsupported", "ORDER BY requires an ordinary column or L2_DISTANCE");
}
struct Candidate { int64_t key; Ref ref; std::vector<Atom> sort; };
struct Better {
    std::vector<bool> descending;
    bool operator()(const Candidate& a, const Candidate& b) const {
        for (size_t i = 0; i < descending.size(); ++i) {
            if (a.sort[i] == b.sort[i]) continue;
            return descending[i] ? a.sort[i] > b.sort[i] : a.sort[i] < b.sort[i];
        }
        return a.key < b.key;
    }
};
size_t candidate_size(const Candidate& c) {
    size_t n = sizeof(Candidate) + c.sort.capacity() * sizeof(Atom) + 32;
    for (const auto& a : c.sort) if (auto s = std::get_if<std::string>(&a)) n += s->capacity();
    return n;
}
void write_candidate(std::ostream& out, const Candidate& c) {
    Json keys = Json::array();
    for (const auto& v : c.sort) std::visit([&](const auto& x) { keys.push_back(x); }, v);
    std::string bytes = Json::array({c.key, c.ref.offset, c.ref.length, std::move(keys)}).dump();
    if (bytes.size() > UINT32_MAX) fail("resource_limit", "sort key too large");
    std::array<unsigned char, 4> length{}; write_le(length.data(), bytes.size(), 4);
    out.write(reinterpret_cast<const char*>(length.data()), length.size());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw IoError("write sort run");
}
bool read_candidate(std::istream& in, Candidate& c) {
    std::array<unsigned char, 4> length{};
    in.read(reinterpret_cast<char*>(length.data()), length.size());
    if (!in.gcount() && in.eof()) return false;
    if (in.gcount() != 4) throw IoError("read sort run length");
    std::string bytes(read_le(length.data(), 4), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<size_t>(in.gcount()) != bytes.size()) throw IoError("read sort run");
    Json j = Json::parse(bytes);
    c.key = j.at(0).get<int64_t>(); c.ref = {j.at(1).get<uint64_t>(), j.at(2).get<uint32_t>()}; c.sort.clear();
    for (const auto& v : j.at(3)) {
        if (v.is_string()) c.sort.emplace_back(v.get<std::string>());
        else if (v.is_number_float()) c.sort.emplace_back(v.get<double>());
        else c.sort.emplace_back(v.get<int64_t>());
    }
    return true;
}
class Sorter {
    static constexpr size_t BUDGET = 8 * 1024 * 1024;
    std::filesystem::path directory_;
    Better better_;
    uint64_t limit_;
    bool heap_;
    size_t bytes_ = 0;
    std::vector<Candidate> items_;
    std::vector<std::filesystem::path> files_, runs_;
    std::filesystem::path new_file() {
        auto p = directory_ / ("sort-" + std::to_string(files_.size()) + ".tmp"); files_.push_back(p); return p;
    }
    void spill() {
        if (items_.empty()) return;
        std::sort(items_.begin(), items_.end(), better_);
        auto p = new_file(); std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) throw IoError("open sort run");
        for (const auto& c : items_) write_candidate(out, c);
        out.close(); if (!out) throw IoError("close sort run");
        runs_.push_back(p); std::vector<Candidate>().swap(items_); bytes_ = 0;
    }
    void merge(const std::vector<std::filesystem::path>& paths, uint64_t limit,
               const std::function<void(const Candidate&)>& emit) {
        struct Entry { Candidate candidate; size_t file; };
        auto later = [&](const Entry& a, const Entry& b) { return better_(b.candidate, a.candidate); };
        std::priority_queue<Entry, std::vector<Entry>, decltype(later)> queue(later);
        std::vector<std::ifstream> inputs; inputs.reserve(paths.size());
        for (const auto& p : paths) {
            inputs.emplace_back(p, std::ios::binary);
            if (!inputs.back()) throw IoError("open sort run for merge");
            Candidate c{}; if (read_candidate(inputs.back(), c)) queue.push({std::move(c), inputs.size() - 1});
        }
        uint64_t count = 0;
        while (!queue.empty() && count < limit) {
            Entry e = queue.top(); queue.pop(); emit(e.candidate); ++count;
            Candidate c{}; if (read_candidate(inputs[e.file], c)) queue.push({std::move(c), e.file});
        }
    }
public:
    Sorter(const std::filesystem::path& dir, Better better, uint64_t limit, size_t maximum_key_size)
        : directory_(dir), better_(std::move(better)), limit_(limit),
          heap_(limit <= 65536 && maximum_key_size <= BUDGET && limit <= BUDGET / std::max(size_t(1), maximum_key_size)) {}
    ~Sorter() { for (const auto& p : files_) { std::error_code e; std::filesystem::remove(p, e); } }
    void add(Candidate c) {
        if (!limit_) return;
        if (heap_) {
            if (items_.size() < limit_) { items_.push_back(std::move(c)); std::push_heap(items_.begin(), items_.end(), better_); }
            else if (better_(c, items_.front())) {
                std::pop_heap(items_.begin(), items_.end(), better_); items_.back() = std::move(c);
                std::push_heap(items_.begin(), items_.end(), better_);
            }
        } else {
            bytes_ += candidate_size(c); items_.push_back(std::move(c));
            if (bytes_ >= BUDGET / 2) spill();
        }
    }
    void finish(const std::function<void(const Candidate&)>& emit) {
        if (runs_.empty()) {
            std::sort(items_.begin(), items_.end(), better_);
            uint64_t n = std::min<uint64_t>(limit_, items_.size());
            for (uint64_t i = 0; i < n; ++i) emit(items_[i]);
            return;
        }
        spill();
        // Keep the number of simultaneously open runs bounded on large tables.
        while (runs_.size() > 32) {
            std::vector<std::filesystem::path> next;
            for (size_t i = 0; i < runs_.size(); i += 32) {
                auto p = new_file(); std::ofstream out(p, std::ios::binary | std::ios::trunc);
                if (!out) throw IoError("open merged sort run");
                std::vector<std::filesystem::path> group(runs_.begin() + i, runs_.begin() + std::min(i + 32, runs_.size()));
                merge(group, limit_, [&](const Candidate& c) { write_candidate(out, c); });
                out.close(); if (!out) throw IoError("close merged sort run");
                for (const auto& old : group) { std::error_code error; std::filesystem::remove(old, error); }
                next.push_back(p);
            }
            runs_ = std::move(next);
        }
        merge(runs_, limit_, emit);
    }
};
}

Result Engine::select(const Statement& s) {
    const Table& table = active_[table_index(s.table)];
    auto predicates = bind_predicates(table, s.conditions);
    std::vector<BoundExpr> projections, ordering;
    if (s.star) {
        for (size_t i = 0; i < table.columns.size(); ++i) projections.push_back({i, {}, false, table.columns[i].name});
    } else for (const auto& e : s.projections) projections.push_back(bind_expr(table, e));
    Better better; size_t maximum_key_size = sizeof(Candidate) + 64;
    for (const auto& o : s.order) {
        std::optional<size_t> alias;
        if (!o.expr.vector) {
            for (size_t i = 0; i < projections.size(); ++i) if (lower(projections[i].name) == lower(o.expr.column)) {
                if (alias) fail("ambiguous_column", "ambiguous ORDER BY alias");
                alias = i;
            }
        }
        BoundExpr bound = alias ? projections[*alias] : bind_expr(table, o.expr);
        if (!bound.distance && table.columns[bound.column].type == Type::Vector) fail("unsupported", "cannot sort vectors directly");
        uint64_t width = bound.distance || table.columns[bound.column].type == Type::Integer ? 16 : uint64_t(table.columns[bound.column].width) * 4;
        maximum_key_size += static_cast<size_t>(std::min<uint64_t>(width + sizeof(Atom) + 64, SIZE_MAX - maximum_key_size));
        ordering.push_back(std::move(bound)); better.descending.push_back(o.descending);
    }
    Result result; result.fields["columns"] = Json::array();
    for (const auto& p : projections) result.fields["columns"].push_back(p.name);
    result.rows = std::make_unique<RowOutput>(directory_);
    uint64_t limit = s.limit.value_or(UINT64_MAX);
    if (!limit) return result;
    std::vector<bool> wanted(table.columns.size(), false);
    for (const auto& p : projections) wanted[p.column] = true;
    for (const auto& p : ordering) wanted[p.column] = true;
    for (const auto& p : predicates) wanted[p.column] = true;
    wanted[table.primary] = false; // The B+ tree already supplies the exact key.
    bool need_row = false, all_payload = true;
    for (size_t i = 0; i < wanted.size(); ++i) if (i != table.primary) {
        need_row |= wanted[i]; all_payload &= wanted[i];
    }
    auto load = [&](int64_t key, Ref ref) {
        Row row;
        if (need_row) row = all_payload ? decode_row(table, storage_.read(ref)) : storage_.read_selected(table, ref, wanted);
        else row.resize(table.columns.size());
        row[table.primary] = key; return row;
    };
    auto output = [&](const Row& row) {
        Json values = Json::array();
        for (const auto& p : projections) {
            if (p.distance) values.push_back(distance(std::get<std::vector<float>>(row[p.column]), p.query));
            else values.push_back(value_json(row[p.column]));
        }
        result.rows->add(values);
    };
    bool vector_query = false;
    for (const auto& p : projections) vector_query |= p.distance;
    for (const auto& o : ordering) vector_query |= o.distance;
    if (vector_query) {
        if (vector_index_ && !vector_index_->valid(table)) vector_index_.reset();
        std::vector<bool> cache_needed(table.columns.size(), false);
        cache_needed[table.primary] = true;
        for (const auto& p : predicates) cache_needed[p.column] = true;
        for (const auto& o : ordering) cache_needed[o.column] = true;
        for (const auto& p : projections) if (p.distance) cache_needed[p.column] = true;
        auto refused = [&](const std::vector<bool>& columns) {
            if (refused_vector_root_ != table.root || refused_vector_table_ != table.name || refused_vector_columns_.size() != columns.size()) return false;
            for (size_t i = 0; i < columns.size(); ++i) if (refused_vector_columns_[i] && !columns[i]) return false;
            return true;
        };
        if ((!vector_index_ || !vector_index_->covers(cache_needed)) && !refused(cache_needed)) {
            auto desired = cache_needed;
            if (vector_index_) for (size_t i = 0; i < desired.size(); ++i) desired[i] = desired[i] || vector_index_->columns()[i];
            if (!refused(desired)) {
                vector_index_.reset(); // Keep the shared budget during rebuilding.
                vector_index_ = VectorIndex::create(storage_, table, desired);
                if (!vector_index_) { refused_vector_root_ = table.root; refused_vector_table_ = table.name; refused_vector_columns_ = std::move(desired); }
            }
        }
        if (vector_index_ && vector_index_->covers(cache_needed) && ordering.size() == 1 && ordering[0].distance && !s.order[0].descending && limit <= 65536) {
            auto hits = vector_index_->search(ordering[0].query, static_cast<size_t>(limit),
                                              [&](const Row& row) { return matches(row, predicates); });
            bool projection_cached = vector_index_->covers(wanted);
            for (const auto& hit : hits) {
                const auto& entry = vector_index_->entries()[hit.entry];
                if (projection_cached) output(entry.row);
                else output(load(entry.key, entry.ref));
            }
            return result;
        }
    }
    BTree tree(storage_);
    uint64_t emitted = 0;
    std::unique_ptr<Sorter> sorter;
    if (!ordering.empty()) sorter = std::make_unique<Sorter>(directory_, better, limit, maximum_key_size);
    auto visit = [&](int64_t key, Ref ref) {
        Row row = load(key, ref);
        if (!matches(row, predicates)) return true;
        if (sorter) {
            Candidate c{key, ref, {}}; c.sort.reserve(ordering.size());
            for (const auto& o : ordering) c.sort.push_back(evaluate(row, o));
            sorter->add(std::move(c));
        } else { output(row); if (++emitted >= limit) return false; }
        return true;
    };
    std::optional<int64_t> point;
    for (const auto& p : predicates) if (p.column == table.primary && p.op == "=") point = std::get<int64_t>(p.value);
    if (point) { if (auto ref = tree.find(table.root, *point)) visit(*point, *ref); }
    else tree.scan(table.root, visit);
    if (sorter) sorter->finish([&](const Candidate& c) { output(load(c.key, c.ref)); });
    return result;
}
}
