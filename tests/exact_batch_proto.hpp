#pragma once
#include <cmath>
#include <cstddef>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define MVD_BATCH_X86 1
#endif

namespace mvd::batch {
constexpr size_t width = 8;
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
    __m256d a = _mm256_setzero_pd(), b = a;
    for (size_t d = 0; d < dimensions; ++d) {
        auto q = _mm256_set1_pd(double(query[d]));
        auto x = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(block + d * width)), q);
        auto y = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(block + d * width + 4)), q);
        a = _mm256_add_pd(a, _mm256_mul_pd(x, x));
        b = _mm256_add_pd(b, _mm256_mul_pd(y, y));
    }
    _mm256_storeu_pd(output, _mm256_sqrt_pd(a));
    _mm256_storeu_pd(output + 4, _mm256_sqrt_pd(b));
}
__attribute__((target("avx512f"))) inline void avx512(const float* block, const float* query, size_t dimensions, double* output) {
    __m512d sums = _mm512_setzero_pd();
    for (size_t d = 0; d < dimensions; ++d) {
        auto points = _mm512_cvtps_pd(_mm256_loadu_ps(block + d * width));
        auto delta = _mm512_sub_pd(points, _mm512_set1_pd(double(query[d])));
        sums = _mm512_add_pd(sums, _mm512_mul_pd(delta, delta));
    }
    _mm512_storeu_pd(output, _mm512_sqrt_pd(sums));
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
