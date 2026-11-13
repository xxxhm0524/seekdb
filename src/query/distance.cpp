#include "distance.h"
#include "exact_norm.h"
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
namespace mvd::detail {
double squared_l2_scalar(const float *a, const float *b, int dim) {
  double sum = 0.0;
  for (int i = 0; i < dim; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return sum;
}
#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2"))) double squared_l2_avx2(const float *a,
                                                          const float *b,
                                                          int dim) {
  __m256d sum0 = _mm256_setzero_pd();
  __m256d sum1 = _mm256_setzero_pd();
  __m256d sum2 = _mm256_setzero_pd();
  __m256d sum3 = _mm256_setzero_pd();
  int i = 0;
  for (; i + 16 <= dim; i += 16) {
    __m256d d0 = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i)),
                               _mm256_cvtps_pd(_mm_loadu_ps(b+i)));
    __m256d d1 = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i+4)),
                               _mm256_cvtps_pd(_mm_loadu_ps(b+i+4)));
    __m256d d2 = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i+8)),
                               _mm256_cvtps_pd(_mm_loadu_ps(b+i+8)));
    __m256d d3 = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i+12)),
                               _mm256_cvtps_pd(_mm_loadu_ps(b+i+12)));
    sum0 = _mm256_add_pd(sum0, _mm256_mul_pd(d0, d0));
    sum1 = _mm256_add_pd(sum1, _mm256_mul_pd(d1, d1));
    sum2 = _mm256_add_pd(sum2, _mm256_mul_pd(d2, d2));
    sum3 = _mm256_add_pd(sum3, _mm256_mul_pd(d3, d3));
  }
  __m256d sum = _mm256_add_pd(_mm256_add_pd(sum0, sum1), _mm256_add_pd(sum2, sum3));
  for (; i + 4 <= dim; i += 4) {
    __m256d va = _mm256_cvtps_pd(_mm_loadu_ps(a + i));
    __m256d vb = _mm256_cvtps_pd(_mm_loadu_ps(b + i));
    __m256d d = _mm256_sub_pd(va, vb);
    sum = _mm256_add_pd(sum, _mm256_mul_pd(d, d));
  }
  double tmp[4];
  _mm256_storeu_pd(tmp, sum);
  double total = tmp[0] + tmp[1] + tmp[2] + tmp[3];
  for (; i < dim; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    total += d * d;
  }
  return total;
}
#endif
double squared_l2_checked(const float *a, const float *b, int dim) {
#if defined(__x86_64__) || defined(_M_X64)
  static const bool has_avx2 =
      __builtin_cpu_supports("avx2");
  if (has_avx2) return squared_l2_avx2(a, b, dim);
#endif
  return squared_l2_scalar(a, b, dim);
}

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2,fma"))) bool graph_distance_exceeds(
    const float *a, const float *b, int dim, float limit) {
  __m256 sum0 = _mm256_setzero_ps();
  __m256 sum1 = _mm256_setzero_ps();
  __m256 sum2 = _mm256_setzero_ps();
  __m256 sum3 = _mm256_setzero_ps();
  int i = 0;
  for (; i + 32 <= dim; i += 32) {
    __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
    __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16));
    __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24));
    sum0 = _mm256_fmadd_ps(d0, d0, sum0);
    sum1 = _mm256_fmadd_ps(d1, d1, sum1);
    sum2 = _mm256_fmadd_ps(d2, d2, sum2);
    sum3 = _mm256_fmadd_ps(d3, d3, sum3);
  }
  __m256 sum = _mm256_add_ps(_mm256_add_ps(sum0, sum1), _mm256_add_ps(sum2, sum3));
  for (; i + 8 <= dim; i += 8) {
    __m256 difference = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    sum = _mm256_fmadd_ps(difference, difference, sum);
  }
  const __m128 halves = _mm_add_ps(_mm256_castps256_ps128(sum), _mm256_extractf128_ps(sum, 1));
  const __m128 pairs = _mm_add_ps(halves, _mm_movehl_ps(halves, halves));
  float total = _mm_cvtss_f32(_mm_add_ss(pairs, _mm_movehdup_ps(pairs)));
  for (; i < dim; ++i) {
    float difference = a[i] - b[i];
    total += difference * difference;
  }
  return total > limit;
}
#endif

DistanceScreen graph_build_screen() {
#if defined(__x86_64__) || defined(_M_X64)
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
    return graph_distance_exceeds;
#endif
  return nullptr;
}

// Promote stored float32 values before subtraction and squaring. All finite
// float32 distances fit in double, including subnormals whose squares would
// silently underflow in a float accumulator.

double returned_l2_distance(const float *a, const float *b, int dim) {
  // L2_DISTANCE is a float64 SQL scalar. Different precise squared norms can
  // round to the same returned Euclidean value, so those rows must tie on PK.
  // Exact dyadic accumulation avoids a second rounding at long-double precision.
  return canonical_l2_distance(a, b, dim);
}


} // namespace mvd::detail
