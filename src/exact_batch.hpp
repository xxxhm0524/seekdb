#pragma once
// Thirty-two candidates share one dimension-major block. Every candidate's double
// accumulation still visits dimensions in scalar order; no fused operations.
#include <cmath>
#include <cstddef>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define MVD_BATCH_X86 1
#endif

namespace mvd::batch {
constexpr size_t width = 32;
using Function = void (*)(const float*, const float*, size_t, double*);
struct Kernel { const char* name; Function evaluate; };
inline void scalar(const float* block, const float* query, size_t dimensions, double* output) {
    double sums[width]{};
    for (size_t d = 0; d < dimensions; ++d) {
        double q = query[d];
        for (size_t lane = 0; lane < width; ++lane) {
            double delta = double(block[d * width + lane]) - q;
            sums[lane] += delta * delta;
        }
    }
    for (size_t lane = 0; lane < width; ++lane) output[lane] = std::sqrt(sums[lane]);
}
#ifdef MVD_BATCH_X86
__attribute__((target("avx2"))) inline void avx2(const float* block, const float* query, size_t dimensions, double* output) {
    __m256d sums[width / 4];
    for (auto& sum : sums) sum = _mm256_setzero_pd();
    for (size_t d = 0; d < dimensions; ++d) {
        auto q = _mm256_set1_pd(double(query[d]));
        for (size_t group = 0; group < width / 4; ++group) {
            auto delta = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(block + d * width + group * 4)), q);
            sums[group] = _mm256_add_pd(sums[group], _mm256_mul_pd(delta, delta));
        }
    }
    for (size_t group = 0; group < width / 4; ++group) _mm256_storeu_pd(output + group * 4, _mm256_sqrt_pd(sums[group]));
}
__attribute__((target("avx512f"))) inline void avx512(const float* block, const float* query, size_t dimensions, double* output) {
    __m512d sums[width / 8];
    for (auto& sum : sums) sum = _mm512_setzero_pd();
    for (size_t d = 0; d < dimensions; ++d) {
        auto q = _mm512_set1_pd(double(query[d]));
        for (size_t group = 0; group < width / 8; ++group) {
            auto points = _mm512_maskz_cvtps_pd(0xff, _mm256_loadu_ps(block + d * width + group * 8));
            auto delta = _mm512_sub_pd(points, q);
            sums[group] = _mm512_add_pd(sums[group], _mm512_mul_pd(delta, delta));
        }
    }
    for (size_t group = 0; group < width / 8; ++group) _mm512_storeu_pd(output + group * 8, _mm512_maskz_sqrt_pd(0xff, sums[group]));
}
#endif
inline Kernel select() {
#ifdef MVD_BATCH_X86
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx2")) return {"avx512", avx512};
    if (__builtin_cpu_supports("avx2")) return {"avx2", avx2};
#endif
    return {"scalar", scalar};
}
}
