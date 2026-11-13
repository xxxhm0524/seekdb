// Synthetic numeric fixtures: no student-learning evidence is implied.
#include "../src/query/distance.h"
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>

struct Record { double estimate; long double truth; int64_t pk; };
static size_t checks=0;

size_t check_graph_screen() {
  const auto screen = mvd::detail::graph_build_screen();
  if (!screen) return 0;

  const float samples[] = {
      0, -0.0f, 0x1p-32f, std::nextafter(0x1p-32f, 1.0f), -0x1p-32f,
      1, std::nextafter(1.0f, 2.0f), -1, 0x1p48f, -0x1p48f};
  std::mt19937 random(20261113);
  std::uniform_int_distribution<size_t> choose(0, std::size(samples) - 1);
  size_t checked = 0;
  for (int dim : {64, 65, 95, 127, 128, 129, 16384}) {
    std::vector<float> a(static_cast<size_t>(dim)), b(a.size());
    const double margin = 1 + (dim + 8) * 0x1p-22;
    for (int trial = 0; trial < 12; ++trial) {
      for (int i = 0; i < dim; ++i) {
        a[i] = samples[choose(random)];
        b[i] = trial % 3 ? samples[choose(random)] : a[i];
      }
      const double exact = mvd::detail::squared_l2_checked(a.data(), b.data(), dim);
      const float equal_limit = static_cast<float>(exact * margin);
      if (screen(a.data(), b.data(), dim, equal_limit))
        throw std::runtime_error("graph screening rejected an equal distance");
      if (exact > 0 &&
          !screen(a.data(), b.data(), dim, static_cast<float>(exact * 0.5 * margin)))
        throw std::runtime_error("graph screening failed to reject a distant candidate");
      checked += exact > 0 ? 2 : 1;
    }
  }
  return checked;
}

void check(const std::vector<Record> &records,const std::vector<size_t> &order,size_t k,uint64_t ulps,bool merge) {
  auto initialize=[&](mvd::detail::CandidateHeap &heap) {
    heap.k=k;heap.uncertainty_ulps=ulps;
    heap.refine=[&](const mvd::detail::Candidate &candidate){return records.at(candidate.pos).truth;};
  };
  mvd::detail::CandidateHeap left,right;initialize(left);initialize(right);
  for(size_t n=0;n<order.size();++n) {
    size_t i=order[n];const auto &record=records[i];
    auto &heap=merge&&n%2 ? right:left;
    heap.push(record.estimate,record.pk,i);
  }
  if(merge)left.merge(right);
  std::vector<size_t> expected(records.size());
  std::iota(expected.begin(),expected.end(),0);
  std::sort(expected.begin(),expected.end(),[&](size_t a,size_t b) {
    return records[a].truth!=records[b].truth ? records[a].truth<records[b].truth : records[a].pk<records[b].pk;
  });
  expected.resize(std::min(k,expected.size()));
  std::sort(left.data.begin(),left.data.end(),[&](const auto&a,const auto&b){return left.less(a,b);});
  if(left.data.size()!=expected.size())throw std::runtime_error("heap count differs from numeric oracle");
  for(size_t i=0;i<expected.size();++i) if(left.data[i].pk!=records[expected[i]].pk)
    throw std::runtime_error("top-K differs from independent precise-value order");
  ++checks;
}

int main() {
  try {
    for(double base: {1e-80,1.0,1e80}) {
      std::vector<Record> records;
      uint64_t bits;std::memcpy(&bits,&base,8);
      for(int n=0;n<200;++n) {
        uint64_t estimate_bits=bits+static_cast<uint64_t>(n%9)-4;
        double estimate;std::memcpy(&estimate,&estimate_bits,8);
        long double truth=static_cast<long double>(base)*(1+std::ldexp(static_cast<long double>(n-100),-60));
        records.push_back({estimate,truth,n-1000});
      }
      for(int n=0;n<200;++n) {
        double distance=base*(2+n);
        records.push_back({distance,static_cast<long double>(distance),1000+n});
      }
      for(unsigned seed=0;seed<25;++seed) {
        std::vector<size_t> order(records.size());std::iota(order.begin(),order.end(),0);
        std::mt19937 random(seed);std::shuffle(order.begin(),order.end(),random);
        for(size_t k: {1,7,32})for(bool merge: {false,true})check(records,order,k,32,merge);
      }
    }
    for(double value: {0.0,std::numeric_limits<double>::max()}) {
      std::vector<Record> records;
      for(int64_t pk: {std::numeric_limits<int64_t>::max(),int64_t(0),int64_t(1),
                       std::numeric_limits<int64_t>::min(),int64_t(-1)})records.push_back({value,value,pk});
      for(uint64_t ulps: {uint64_t(0),uint64_t(32),std::numeric_limits<uint64_t>::max()})
        for(bool merge: {false,true})check(records,{0,1,2,3,4},2,ulps,merge);
    }
    // A candidate exactly on the upper ULP boundary can have a better precise
    // distance. Changing either early rejection from > to >= must fail here.
    double base=1.0;uint64_t bits;std::memcpy(&bits,&base,8);
    auto value=[&](uint64_t offset){double d;uint64_t b=bits+offset;std::memcpy(&d,&b,8);return d;};
    long double center=static_cast<long double>(value(16));
    std::vector<Record> boundary={{base,center,100},{value(33),value(33),200},
        {value(32),center-std::ldexp(1.0L,-60),0},{value(31),center-std::ldexp(1.0L,-59),1}};
    for(bool merge: {false,true})check(boundary,{0,1,2,3},1,32,merge);
    const size_t graph_checks = check_graph_screen();
    std::cout<<"{\"passed\":true,\"checks\":"<<checks
      <<",\"graph_screen_checks\":"<<graph_checks
      <<",\"oracle\":\"precise numeric order and primary keys; includes merge and ULP boundary\"}\n";
    return 0;
  } catch(const std::exception &error) {
    std::cerr<<error.what()<<'\n';return 1;
  }
}
