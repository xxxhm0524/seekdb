#include "vector_index.hpp"
#include <bit>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <random>

using namespace mvd;
static void need(bool yes, const char* reason) { if (!yes) throw std::runtime_error(reason); }
static void same(const std::vector<VectorIndex::Hit>& a, const std::vector<VectorIndex::Hit>& b) {
    need(a.size() == b.size(), "result count");
    for (size_t i = 0; i < a.size(); ++i)
        need(a[i].entry == b[i].entry && std::bit_cast<uint64_t>(a[i].distance) == std::bit_cast<uint64_t>(b[i].distance), "exact distance bits or tie order");
}

int main(int argc, char** argv) try {
    need(argc == 2, "usage: probe EMPTY_DIRECTORY");
    const std::filesystem::path root = argv[1]; need(!std::filesystem::exists(root), "existing output");
    std::filesystem::create_directories(root);
    ::setenv("MVD_READ_MAP_MIB", "64", 1);
    std::mt19937 rng(20261116); std::uniform_real_distribution<float> value(-1, 1);
    size_t checked = 0, mapped_certified = 0, tight_fallbacks = 0, copied_fallbacks = 0;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        const bool tight = scenario == 1;
        const size_t count = scenario == 0 ? 10000 : tight ? 3000 : 513;
        const uint32_t dimensions = scenario < 2 ? 128 : 33;
        Table table; table.name = "t";
        size_t vector_column, primary, group, body;
        if (scenario < 2) {
            table.columns = {{"id", Type::Integer, 0}, {"g", Type::Integer, 0}, {"body", Type::Text, 64}, {"v", Type::Vector, dimensions}};
            primary = 0; group = 1; body = 2; vector_column = 3;
        } else if (scenario == 2) {
            table.columns = {{"v", Type::Vector, dimensions}, {"body", Type::Text, 64}, {"g", Type::Integer, 0}, {"id", Type::Integer, 0}};
            vector_column = 0; body = 1; group = 2; primary = 3;
        } else {
            table.columns = {{"body", Type::Text, 64}, {"id", Type::Integer, 0}, {"v", Type::Vector, dimensions}, {"g", Type::Integer, 0}};
            body = 0; primary = 1; vector_column = 2; group = 3;
        }
        table.primary = primary;
        std::unique_ptr<VectorIndex> mapped, control;
        const auto directory = root / std::to_string(scenario); std::filesystem::create_directory(directory);
        {
            Storage store(directory); BTree tree(store);
            std::vector<size_t> order(count); std::iota(order.begin(), order.end(), 0); std::shuffle(order.begin(), order.end(), rng);
            for (size_t id : order) {
                const int64_t key = id == 0 ? INT64_MIN : id == 1 ? INT64_MAX : int64_t(id) - 2;
                Row row(table.columns.size()); row[primary] = key; row[group] = int64_t(id % 9);
                row[body] = std::string(id % 13, 'x') + std::string("a\0b", 3) + "\xe4\xb8\xad\xf0\x9f\x98\x80";
                std::vector<float> vector(dimensions);
                for (float& x : vector) x = tight ? 1.0f : value(rng);
                if (tight && id % 3) vector[id % dimensions] = std::nextafter(1.0f, 2.0f);
                if (!tight && id < 3) std::fill(vector.begin(), vector.end(), id % 2 ? -0.0f : 0.0f);
                row[vector_column] = std::move(vector);
                Ref ref = store.append(encode_row(table, row)); tree.put(table.root, key, ref);
                ++table.count; table.live_bytes += ref.length;
            }
            store.commit({table}); store.prepare_read_mapping();
            need(bool(store.read_mapping()), "read mapping absent");
            std::vector<bool> wanted(table.columns.size(), true); wanted[body] = false;
            ::setenv("MVD_VECTOR_CACHE_MIB", tight ? "1" : "16", 1);
            mapped = VectorIndex::create(store, table, wanted, group);
            need(bool(mapped) && mapped->uses_mapped_source() && mapped->owned_coordinate_bytes() == 0, "source was copied at preparation");
            store.release_read_mapping(); // The index now owns the mapping lifetime.
            ::setenv("MVD_VECTOR_CACHE_MIB", "64", 1);
            control = VectorIndex::create(store, table, wanted, group);
            need(bool(control) && !control->uses_mapped_source() && control->owned_coordinate_bytes() > 0, "control did not copy independent positional reads");
        } // The descriptor and Storage are gone; both indexes must remain usable.
        for (size_t trial = 0; trial < (tight ? 8 : 20); ++trial) {
            std::vector<float> query(dimensions);
            for (float& x : query) x = tight ? 0.0f : value(rng);
            VectorIndex::SearchStats stats;
            same(mapped->search(query, 10, {}, 0, count, &stats), control->search(query, 10, {}, 0, count, nullptr, false)); ++checked;
            if (tight) {
                need(stats.screen_attempted && !stats.screen_certified && mapped->uses_mapped_source() && mapped->owned_coordinate_bytes() == 0, "tight fallback copied beyond its budget");
                ++tight_fallbacks;
            } else if (scenario == 0) {
                need(stats.screen_certified && mapped->uses_mapped_source() && mapped->owned_coordinate_bytes() == 0, "normal mapped certificate path not exercised");
                ++mapped_certified;
            }
            auto bounds = mapped->equal_bounds(group, Value(int64_t(trial % 9))); need(bool(bounds), "cluster range missing");
            same(mapped->search(query, 10, {}, bounds->first, bounds->second), control->search(query, 10, {}, bounds->first, bounds->second, nullptr, false)); ++checked;
            auto predicate = [group](const Row& row) { return std::get<int64_t>(row[group]) >= 4; };
            same(mapped->search(query, 10, predicate), control->search(query, 10, predicate, 0, count, nullptr, false)); ++checked;
        }
        if (!tight) {
            std::vector<float> query(dimensions, 0.125f); VectorIndex::SearchStats stats;
            same(mapped->search(query, 17, {}, 0, count, &stats), control->search(query, 17, {}, 0, count, nullptr, false)); ++checked;
            need(!mapped->uses_mapped_source() && mapped->owned_coordinate_bytes() > 0, "full scan did not materialize within budget"); ++copied_fallbacks;
            for (size_t i : {size_t(0), count / 2, count - 1}) {
                Row a = mapped->materialize(i), b = control->materialize(i);
                need(a.size() == table.columns.size() && a == b, "materialization or layout"); ++checked;
            }
        }
    }
    // Pin a committed view across descriptor close and atomic replacement.
    {
        const auto path = root / "lifetime"; std::filesystem::create_directory(path);
        std::shared_ptr<const ReadMapping> pinned;
        {
            File file(path / "database.mvd"); std::string old(8192, 'a'); file.write(0, old.data(), old.size());
            file.prepare_read_mapping(old.size()); pinned = file.read_mapping(); need(bool(pinned), "pin absent");
            {
                File replacement(path / "replacement.mvd"); std::string next(8192, 'b'); replacement.write(0, next.data(), next.size()); replacement.sync();
            }
            file.replace_with(path / "replacement.mvd"); need(pinned->data()[0] == 'a' && pinned->data()[8191] == 'a', "atomic replacement invalidated pinned source");
            char current = 0; file.read(0, &current, 1); need(current == 'b', "new descriptor has old data");
        }
        need(pinned->data()[0] == 'a' && pinned->data()[8191] == 'a', "descriptor close invalidated pinned source");
    }
    std::cout << "{\"passed\":true,\"exact_comparisons\":" << checked << ",\"mapped_certified\":" << mapped_certified
              << ",\"budget_preserving_fallbacks\":" << tight_fallbacks << ",\"copied_fallbacks\":" << copied_fallbacks
              << ",\"descriptor_close_and_replacement_lifetime\":true,\"unaligned_unicode_nul_layouts\":true}\n";
} catch (const std::exception& error) {
    std::cout << Json({{"passed", false}, {"error", error.what()}}).dump() << '\n'; return 1;
}
