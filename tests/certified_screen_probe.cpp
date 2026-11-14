#include "../src/quantized_screen.hpp"
#include "../src/vector_index.hpp"
#include <bit>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

using namespace mvd;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void same(const std::vector<VectorIndex::Hit>& a, const std::vector<VectorIndex::Hit>& b) {
    require(a.size() == b.size(), "certificate result count");
    for (size_t i = 0; i < a.size(); ++i)
        require(a[i].entry == b[i].entry && std::bit_cast<uint64_t>(a[i].distance) == std::bit_cast<uint64_t>(b[i].distance), "certificate exact bits or ties");
}
int main(int argc, char** argv) try {
    require(argc == 2, "usage: probe EMPTY_DIRECTORY");
    const std::filesystem::path directory = argv[1];
    require(!std::filesystem::exists(directory), "probe directory already exists");
    std::filesystem::create_directories(directory);
    std::mt19937 rng(984131); size_t bounds_checked = 0, searches_checked = 0, certified = 0, fallbacks = 0;
    std::vector<unsigned> kernels{0, 1};
#ifdef MVD_BATCH_X86
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) kernels.push_back(2);
    if (__builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("avx512f")) kernels.push_back(3);
#endif
    for (size_t dimensions : {32u, 33u, 128u, 1024u, 4096u}) for (unsigned trial = 0; trial < 12; ++trial) {
        constexpr size_t count = 257;
        std::vector<float> rows(count * dimensions), query(dimensions);
        for (float& x : rows) x = trial % 4 == 3 ? std::bit_cast<float>(uint32_t(rng()) & 0xfeffffffU) : std::ldexp(float(int(rng() % 1024) - 512) / 256, int(trial % 3) * 100 - 100);
        for (float& x : query) x = trial % 4 == 3 ? std::bit_cast<float>(uint32_t(rng()) & 0xfeffffffU) : std::ldexp(float(int(rng() % 1024) - 512) / 256, int(trial % 3) * 100 - 100);
        if (trial % 4 == 3) { rows[0] = std::numeric_limits<float>::max(); rows[1] = -rows[0]; }
        rows[2] = std::numeric_limits<float>::denorm_min(); rows[3] = -0.0f;
        auto index = QuantizedScreen::create(count, dimensions, 16 * 1024 * 1024,
            [&](size_t row, size_t d) { return rows[row * dimensions + d]; });
        if (!QuantizedScreen::supported(dimensions)) { require(!index, "unsupported screen was built"); continue; }
        require(bool(index), "numeric screen construction refused"); const auto encoded = index->query(query);
        for (size_t first = 0; first < count; first += QuantizedScreen::width) {
            std::array<int32_t, QuantizedScreen::width> portable{}; index->scores(encoded, first, portable, 1);
            for (const auto kernel : kernels) {
                std::array<int32_t, QuantizedScreen::width> scores{}; index->scores(encoded, first, scores, kernel);
                require(scores == portable, "integer SIMD mismatch");
                for (size_t lane = 0; lane < QuantizedScreen::width && first + lane < count; ++lane) {
                    require(scores[lane] >= 0, "negative integer square"); double squared = 0;
                    for (size_t d = 0; d < dimensions; ++d) { double delta = double(rows[(first + lane) * dimensions + d]) - double(query[d]); squared += delta * delta; }
                    require(index->lower(encoded, scores[lane]) <= index->reference_upper(std::sqrt(squared)), "integer bound exceeds reference envelope"); ++bounds_checked;
                }
            }
        }
    }
    std::uniform_real_distribution<float> random(-1, 1);
    double screened_seconds = 0, full_seconds = 0; size_t timed_exact_rows = 0, timed_screened_rows = 0, timed_rowwise_queries = 0, reblocks = 0;
    for (size_t dimensions : {3u, 33u, 128u, 1024u, 4096u}) {
        const size_t count = dimensions == 128 ? 10000 : 257;
        auto path = directory / std::to_string(dimensions); std::filesystem::create_directory(path);
        Storage storage(path);
        Table table; table.name = "t"; table.primary = 1;
        table.columns = {{"category", Type::Text, 40}, {"id", Type::Integer, 0}, {"bucket", Type::Integer, 0}, {"v", Type::Vector, uint32_t(dimensions)}};
        if (dimensions < 32) table.indexes.push_back("idx");
        const std::array<std::string, 7> groups{"a", "\xc3\xa9", "\xe4\xb8\xad", "z", std::string("n\0x", 3), "", "aa"};
        BTree tree(storage);
        for (size_t i = 0; i < count; ++i) {
            std::vector<float> vector(dimensions); for (float& x : vector) x = random(rng);
            if (i < 3) std::fill(vector.begin(), vector.end(), 0.0f);
            const int64_t key = i == 0 ? INT64_MIN : i == 1 ? INT64_MAX : int64_t(i) - 3;
            Row row{groups[i % groups.size()], key, int64_t(i % 9) - 4, vector};
            Ref ref = storage.append(encode_row(table, row)); tree.put(table.root, key, ref); ++table.count; table.live_bytes += ref.length;
        }
        storage.commit({table});
        auto index = VectorIndex::create(storage, table, {true, true, true, true}, 0);
        require(bool(index), "probe cache refused");
        for (size_t i = 1; i < index->entries().size(); ++i) {
            const auto& a = index->entries()[i - 1]; const auto& b = index->entries()[i];
            require(a.row[0] < b.row[0] || (a.row[0] == b.row[0] && a.key < b.key), "physical cluster order");
        }
        for (unsigned trial = 0; trial < 12; ++trial) {
            std::vector<float> query(dimensions); for (float& x : query) x = trial % 4 ? random(rng) : 0;
            for (size_t k : {1u, 10u, 16u, 17u, 32u, 300u}) for (unsigned filter = 0; filter < 3; ++filter) {
                size_t begin = 0, end = count; std::function<bool(const Row&)> predicate;
                if (filter == 1) { auto bounds = index->equal_bounds(0, groups[trial % groups.size()]); require(bool(bounds), "cluster bounds unavailable"); begin = bounds->first; end = bounds->second; }
                if (filter == 2) predicate = [](const Row& row) { return std::get<int64_t>(row[2]) >= 0; };
                VectorIndex::SearchStats stats;
                auto actual = index->search(query, k, predicate, begin, end, &stats);
                VectorIndex::SearchStats control;
                auto expected = index->search(query, k, predicate, begin, end, &control, false);
                reblocks += stats.source_reblocked + control.source_reblocked;
                require(!control.source_reblocked || control.rowwise_source, "reblocked an already blocked snapshot");
                same(actual, expected); ++searches_checked;
                certified += stats.screen_certified; fallbacks += stats.screen_attempted && !stats.screen_certified;
            }
        }
        if (dimensions == 128) {
            std::vector<std::vector<float>> queries(200, std::vector<float>(dimensions));
            for (auto& query : queries) for (float& x : query) x = random(rng);
            auto start = std::chrono::steady_clock::now();
            std::vector<std::vector<VectorIndex::Hit>> expected;
            for (const auto& query : queries) expected.push_back(index->search(query, 10, {}, 0, count, nullptr, false));
            full_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            // Recreate the same deterministic row/key order so this measures
            // the reference's contiguous-source certificate layout as used.
            index.reset(); index = VectorIndex::create(storage, table, {true, true, true, true}, 0);
            require(bool(index), "recreated timed cache refused");
            start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < queries.size(); ++i) {
                VectorIndex::SearchStats stats; same(index->search(queries[i], 10, {}, 0, count, &stats), expected[i]);
                if (QuantizedScreen::supported(dimensions)) {
                    require(stats.screen_certified && stats.exact_rows <= 32, "normal workload did not certify bounded candidates");
                }
                timed_exact_rows += stats.exact_rows; timed_screened_rows += stats.screened_rows;
                timed_rowwise_queries += stats.rowwise_source;
            }
            screened_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (QuantizedScreen::supported(dimensions))
                require(timed_exact_rows < queries.size() * 32, "normal screening did not avoid remaining exact candidates");
            if (QuantizedScreen::supported(dimensions)) require(timed_rowwise_queries == queries.size(), "contiguous-source candidate path was not exercised");
        }
        // Unsupported rounding modes must decline the certificate, without mutation.
        const int previous = std::fegetround(); std::fesetround(FE_DOWNWARD);
        require(!QuantizedScreen::supported(dimensions), "certificate admitted unsupported rounding"); std::fesetround(previous);
    }
    {
        const size_t dimensions = 128, count = 321;
        auto path = directory / "near-ties"; std::filesystem::create_directory(path); Storage storage(path);
        Table table; table.name = "ties"; table.columns = {{"id", Type::Integer, 0}, {"v", Type::Vector, uint32_t(dimensions)}};
        BTree tree(storage);
        for (size_t i = 0; i < count; ++i) {
            std::vector<float> vector(dimensions, 1.0f);
            if (i % 3) vector[i % dimensions] = std::nextafter(1.0f, 2.0f);
            Ref ref = storage.append(encode_row(table, Row{int64_t(count - i), vector}));
            tree.put(table.root, int64_t(count - i), ref); ++table.count; table.live_bytes += ref.length;
        }
        storage.commit({table}); auto index = VectorIndex::create(storage, table);
        require(bool(index), "near-tie cache refused");
        for (float value : {0.0f, 1.0f, std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}) {
            std::vector<float> query(dimensions, value); VectorIndex::SearchStats stats;
            same(index->search(query, 10, {}, 0, count, &stats), index->search(query, 10, {}, 0, count, nullptr, false));
            if (QuantizedScreen::supported(dimensions)) require(stats.screen_attempted && !stats.screen_certified, "near-tie or clipped-query certificate must fall back");
            ++searches_checked; fallbacks += stats.screen_attempted && !stats.screen_certified;
        }
    }
    std::cout << "{\"passed\":true,\"bound_checks\":" << bounds_checked << ",\"searches_checked\":" << searches_checked
              << ",\"certified\":" << certified << ",\"fallbacks\":" << fallbacks << ",\"timed_full_seconds\":" << full_seconds
              << ",\"timed_screen_seconds\":" << screened_seconds << ",\"timed_exact_rows\":" << timed_exact_rows
              << ",\"timed_screened_rows\":" << timed_screened_rows << ",\"timed_rowwise_queries\":" << timed_rowwise_queries << ",\"full_scan_reblocks\":" << reblocks << "}\n";
} catch (const std::exception& error) {
    std::cout << Json({{"passed", false}, {"error", error.what()}}).dump() << '\n'; return 1;
}
