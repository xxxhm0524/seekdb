#pragma once
#include "exact_batch.hpp"
#include <algorithm>
#include <array>
#include <cfenv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace mvd {
// An auxiliary rejection certificate. Original float32 coordinates remain the
// authority, and every returned distance still uses the full binary64 loop.
class QuantizedScreen {
public:
    static constexpr size_t width = 32;
    struct Query { std::vector<int8_t> codes; int32_t norm = 0, sum = 0; double error = 0; };
private:
    size_t dimensions_ = 0, groups_ = 0;
    double scale_ = 1, maximum_error_ = 0;
    std::vector<uint8_t> codes_;
    std::vector<int32_t> norms_;
    static double up(double value) { return std::nextafter(value, std::numeric_limits<double>::infinity()); }
    static double down(double value) { return std::nextafter(value, -std::numeric_limits<double>::infinity()); }
    int encode(double value) const { return int(std::clamp(std::round(value / scale_), -127.0, 127.0)); }
    double add_error(double sum, double value, int code) const {
        const double represented = double(code) * scale_;
        const double allowance = (std::abs(value) + std::abs(represented)) * 0x1p-48 + std::numeric_limits<double>::min();
        const double error = up(std::abs(value - represented) + allowance);
        return up(sum + up(error * error));
    }
    using Function = void (*)(const uint8_t*, const int8_t*, size_t, int32_t*);
    Function function_ = nullptr;
    static void scalar(const uint8_t* rows, const int8_t* query, size_t groups, int32_t* output) {
        for (size_t lane = 0; lane < width; ++lane) output[lane] = 0;
        for (size_t group = 0; group < groups; ++group)
            for (size_t lane = 0; lane < width; ++lane)
                for (size_t d = 0; d < 4; ++d) output[lane] += int32_t(rows[group * width * 4 + lane * 4 + d]) * int32_t(query[group * 4 + d]);
    }
#ifdef MVD_BATCH_X86
    __attribute__((target("avx2"))) static void avx2(const uint8_t* rows, const int8_t* query, size_t groups, int32_t* output) {
        __m256i sums[width / 8]; for (auto& sum : sums) sum = _mm256_setzero_si256();
        const auto ones = _mm256_set1_epi16(1), low_mask = _mm256_set1_epi8(127);
        for (size_t group = 0; group < groups; ++group) {
            int32_t packed; std::memcpy(&packed, query + group * 4, 4);
            // Split the unsigned row byte into <=127 and <=128 components.
            // Each signed-16 pair is <=32512 in magnitude; maddubs cannot
            // saturate. Combine only after widening each pair sum to int32.
            const auto q = _mm256_set1_epi32(packed);
            for (size_t part = 0; part < width / 8; ++part) {
                const auto values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows + group * width * 4 + part * 32));
                const auto low = _mm256_maddubs_epi16(_mm256_and_si256(values, low_mask), q);
                const auto high = _mm256_maddubs_epi16(_mm256_andnot_si256(low_mask, values), q);
                sums[part] = _mm256_add_epi32(sums[part], _mm256_add_epi32(_mm256_madd_epi16(low, ones), _mm256_madd_epi16(high, ones)));
            }
        }
        for (size_t part = 0; part < width / 8; ++part) _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + part * 8), sums[part]);
    }
    __attribute__((target("avx512f,avx512vnni"))) static void vnni(const uint8_t* rows, const int8_t* query, size_t groups, int32_t* output) {
        __m512i sums[width / 16]; for (auto& sum : sums) sum = _mm512_setzero_si512();
        for (size_t group = 0; group < groups; ++group) {
            int32_t packed; std::memcpy(&packed, query + group * 4, 4);
            const auto q = _mm512_set1_epi32(packed);
            for (size_t part = 0; part < width / 16; ++part)
                sums[part] = _mm512_dpbusd_epi32(sums[part], _mm512_loadu_si512(rows + group * width * 4 + part * 64), q);
        }
        for (size_t part = 0; part < width / 16; ++part) _mm512_storeu_si512(output + part * 16, sums[part]);
    }
#endif
    static Function select() {
#ifdef MVD_BATCH_X86
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("avx512f")) return vnni;
        if (__builtin_cpu_supports("avx2")) return avx2;
#endif
        return scalar;
    }
public:
    static bool supported(size_t dimensions) {
        if (dimensions < 32 || dimensions > 4096 || std::fegetround() != FE_TONEAREST) return false;
#ifdef MVD_BATCH_X86
        return (_mm_getcsr() & 0xffc0U) == 0x1f80U;
#else
        return false;
#endif
    }
    static std::unique_ptr<QuantizedScreen> create(size_t count, size_t dimensions, size_t budget,
            const std::function<float(size_t, size_t)>& coordinate) {
        if (!supported(dimensions)) return nullptr;
        const size_t groups = (dimensions + 3) / 4, blocks = (count + width - 1) / width;
        const size_t fixed = sizeof(QuantizedScreen) + groups * 4 + 2048;
        if (fixed > budget || blocks > (budget - fixed) / (groups * width * 4 + width * sizeof(int32_t))) return nullptr;
        auto index = std::make_unique<QuantizedScreen>(); index->dimensions_ = dimensions; index->groups_ = groups;
        index->function_ = select();
        index->codes_.assign(blocks * groups * width * 4, 128); index->norms_.assign(blocks * width, 0);
        if (fixed + index->codes_.capacity() + index->norms_.capacity() * sizeof(int32_t) > budget) return nullptr;
        double maximum = 0;
        for (size_t row = 0; row < count; ++row) for (size_t d = 0; d < dimensions; ++d)
            maximum = std::max(maximum, std::abs(double(coordinate(row, d))));
        if (!maximum) return nullptr;
        index->scale_ = maximum / 127;
        for (size_t row = 0; row < count; ++row) {
            double error = 0; int32_t norm = 0;
            for (size_t d = 0; d < dimensions; ++d) {
                const double value = coordinate(row, d); const int code = index->encode(value);
                index->codes_[(row / width) * groups * width * 4 + (d / 4) * width * 4 + (row % width) * 4 + d % 4] = uint8_t(code + 128);
                norm += code * code; error = index->add_error(error, value, code);
            }
            index->norms_[row] = norm; index->maximum_error_ = std::max(index->maximum_error_, up(std::sqrt(error)));
        }
        return index;
    }
    Query query(const std::vector<float>& vector) const {
        Query result; result.codes.assign(groups_ * 4, 0); double error = 0;
        for (size_t d = 0; d < dimensions_; ++d) {
            const int code = encode(vector[d]); result.codes[d] = int8_t(code);
            result.norm += code * code; result.sum += code; error = add_error(error, vector[d], code);
        }
        result.error = up(std::sqrt(error)); return result;
    }
    void scores(const Query& query, size_t first, std::array<int32_t, width>& output, unsigned implementation = 0) const {
        auto function = implementation == 1 ? scalar : function_;
#ifdef MVD_BATCH_X86
        if (implementation == 2) function = avx2;
        if (implementation == 3) function = vnni;
#endif
        function(codes_.data() + (first / width) * groups_ * width * 4, query.codes.data(), groups_, output.data());
        for (size_t lane = 0; lane < width; ++lane)
            output[lane] = norms_[first + lane] + query.norm + 256 * query.sum - 2 * output[lane];
    }
    double lower(const Query& query, int32_t squared_score) const {
        if (squared_score <= 0) return 0;
        double distance = down(scale_ * down(std::sqrt(double(squared_score))));
        distance = down(distance - maximum_error_); distance = down(distance - query.error);
        return std::max(0.0, distance);
    }
    double reference_upper(double distance) const {
        // Float32 differences and their squares cannot underflow binary64.
        // This envelope covers the dimension-ordered RN64 subtraction/product/
        // sum and the reported sqrt rounding; compare strictly to retain ties.
        return up(up(distance) * (1 + std::ldexp(double(dimensions_ + 8), -48)));
    }
};
}
