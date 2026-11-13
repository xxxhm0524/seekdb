#pragma once
#include "../common.h"
#include <functional>
namespace mvd::detail {
using DistanceScreen=bool(*)(const float*,const float*,int,float);
double squared_l2_scalar(const float*,const float*,int);
double squared_l2_checked(const float*,const float*,int);
double returned_l2_distance(const float*,const float*,int);
DistanceScreen graph_build_screen();
struct Candidate {
  double dist;
  int64_t pk;
  uint64_t pos;
  mutable bool precise_ready = false;
  mutable long double precise = 0;
};
// Max-heap where the top is the worst candidate.
struct CandidateHeap {
  std::vector<Candidate> data;
  double reject_above = std::numeric_limits<double>::infinity();
  // Configure k and uncertainty_ulps before the first insertion.
  size_t k = 10;
  uint64_t uncertainty_ulps = 0;
  std::function<long double(const Candidate &)> refine;
  long double precise(const Candidate &candidate) const {
    if (!candidate.precise_ready) {
      candidate.precise = refine(candidate);
      candidate.precise_ready = true;
    }
    return candidate.precise;
  }
  bool less(const Candidate &a, const Candidate &b) const {
    if (a.dist == 0 && b.dist == 0) return a.pk < b.pk;
    // Squared distances are nonnegative finite doubles. Their bit patterns
    // are ordered, so a conservative ULP band avoids extra work for ordinary
    // queries. The refinement callback uses the canonical returned L2 value,
    // including square-root rounding ties, rather than a hidden squared key.
    uint64_t first, second;
    std::memcpy(&first, &a.dist, sizeof(first));
    std::memcpy(&second, &b.dist, sizeof(second));
    uint64_t gap = first > second ? first - second : second - first;
    if (gap > uncertainty_ulps) return a.dist < b.dist;
    long double left = precise(a), right = precise(b);
    if (left != right) return left < right;
    return a.pk < b.pk;
  }
  void refresh_rejection_bound() {
    if (data.size() < k) return;
    uint64_t bits, maximum;
    const double largest = std::numeric_limits<double>::max();
    std::memcpy(&bits, &data.front().dist, sizeof(bits));
    std::memcpy(&maximum, &largest, sizeof(maximum));
    if (bits > maximum || uncertainty_ulps > maximum - bits) {
      reject_above = std::numeric_limits<double>::infinity();
    } else {
      bits += uncertainty_ulps;
      std::memcpy(&reject_above, &bits, sizeof(bits));
    }
  }
  void push(Candidate candidate) {
    if (candidate.dist > reject_above) return;
    auto compare = [this](const Candidate &a, const Candidate &b) { return less(a, b); };
    if (data.size() < k) {
      data.push_back(std::move(candidate));
      std::push_heap(data.begin(), data.end(), compare);
      refresh_rejection_bound();
    } else if (less(candidate, data.front())) {
      std::pop_heap(data.begin(), data.end(), compare);
      data.back() = std::move(candidate);
      std::push_heap(data.begin(), data.end(), compare);
      refresh_rejection_bound();
    }
  }
  void push(double dist, int64_t pk, uint64_t pos) {
    // Skip only values beyond the existing comparator's entire ULP band.
    if (dist > reject_above) return;
    push(Candidate{dist, pk, pos});
  }
  void merge(CandidateHeap &other) {
    for (const auto &c : other.data) push(c);
  }
};
}
