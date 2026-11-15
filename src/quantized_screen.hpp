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
    enum class Backend : uint8_t { Scalar, Avx2, Vnni };
    Backend backend_ = Backend::Scalar;
    static void scalar(const uint8_t* rows, const int8_t* query, size_t groups, int32_t, int32_t* output) {
        for (size_t lane = 0; lane < width; ++lane) output[lane] = 0;
        for (size_t group = 0; group < groups; ++group)
            for (size_t lane = 0; lane < width; ++lane)
                for (size_t d = 0; d < 4; ++d) output[lane] += int32_t(rows[group * width * 4 + lane * 4 + d]) * int32_t(query[group * 4 + d]);
    }
#ifdef MVD_BATCH_X86
    __attribute__((target("avx2"))) static int32_t avx2_fused(const uint8_t* rows, const int8_t* query, size_t groups, int32_t, int32_t query_norm, const int32_t* norms, int32_t* output) {
        __m256i sums[width / 8]; for (auto& sum : sums) sum = _mm256_setzero_si256();
        const auto ones = _mm256_set1_epi16(1);
        const auto bias = _mm256_set1_epi8(static_cast<char>(0x80));
        for (size_t group = 0; group < groups; ++group) {
            int32_t packed; std::memcpy(&packed, query + group * 4, 4);
            const auto q = _mm256_set1_epi32(packed);
            const auto magnitude = _mm256_abs_epi8(q);
            for (size_t part = 0; part < width / 8; ++part) {
                const auto encoded = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows + group * width * 4 + part * 32));
                const auto values = _mm256_xor_si256(encoded, bias);
                // Both signed codes remain in [-127,127]. A pair's magnitude
                // is <=32258, so maddubs cannot saturate signed 16-bit lanes.
                const auto pairs = _mm256_maddubs_epi16(magnitude, _mm256_sign_epi8(values, q));
                sums[part] = _mm256_add_epi32(sums[part], _mm256_madd_epi16(pairs, ones));
            }
        }
        const auto query_square = _mm256_set1_epi32(query_norm);
        auto minimum = _mm256_set1_epi32(std::numeric_limits<int32_t>::max());
        for (size_t part = 0; part < width / 8; ++part) {
            // sums are already centered, so the old unsigned-row bias cancels.
            const auto score = _mm256_sub_epi32(_mm256_add_epi32(query_square,
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(norms + part * 8))), _mm256_slli_epi32(sums[part], 1));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + part * 8), score);
            minimum = _mm256_min_epi32(minimum, score);
        }
        auto reduced = _mm_min_epi32(_mm256_castsi256_si128(minimum), _mm256_extracti128_si256(minimum, 1));
        reduced = _mm_min_epi32(reduced, _mm_shuffle_epi32(reduced, _MM_SHUFFLE(2,3,0,1)));
        reduced = _mm_min_epi32(reduced, _mm_shuffle_epi32(reduced, _MM_SHUFFLE(1,0,3,2)));
        return _mm_cvtsi128_si32(reduced);
    }
    __attribute__((target("avx512f,avx512vnni"))) static void vnni(const uint8_t* rows, const int8_t* query, size_t groups, int32_t, int32_t* output) {
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
    static Backend select() {
#ifdef MVD_BATCH_X86
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("avx512f")) return Backend::Vnni;
        if (__builtin_cpu_supports("avx2")) return Backend::Avx2;
#endif
        return Backend::Scalar;
    }
public:
    size_t bytes() const { return sizeof(QuantizedScreen) + codes_.capacity() + norms_.capacity() * sizeof(int32_t) + groups_ * 4 + 2048; }
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
        index->backend_ = select();
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
    int32_t scores(const Query& query, size_t first, std::array<int32_t, width>& output, unsigned implementation = 0) const {
        Backend backend = implementation == 1 ? Backend::Scalar : backend_;
#ifdef MVD_BATCH_X86
        if (implementation == 2) backend = Backend::Avx2;
        if (implementation == 3) backend = Backend::Vnni;
#endif
        const auto* rows = codes_.data() + (first / width) * groups_ * width * 4;
#ifdef MVD_BATCH_X86
        if (backend == Backend::Avx2)
            return avx2_fused(rows, query.codes.data(), groups_, query.sum, query.norm, norms_.data() + first, output.data());
        // Preserve the existing VNNI dot product and its original common
        // normalization. Only the natively verified AVX2 path is fused.
        if (backend == Backend::Vnni) vnni(rows, query.codes.data(), groups_, query.sum, output.data());
        else
#endif
            scalar(rows, query.codes.data(), groups_, query.sum, output.data());
        for (size_t lane = 0; lane < width; ++lane)
            output[lane] = norms_[first + lane] + query.norm + 256 * query.sum - 2 * output[lane];
        return *std::min_element(output.begin(), output.end());
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
