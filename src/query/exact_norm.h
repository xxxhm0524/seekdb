#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mvd::norm_detail {
static_assert(sizeof(float)==4&&std::numeric_limits<float>::is_iec559&&std::numeric_limits<float>::digits==24);
static_assert(sizeof(double)==8&&std::numeric_limits<double>::is_iec559&&std::numeric_limits<double>::digits==53);
// Every finite binary32 value is a signed integer times a power of two.
// Accumulate a*a+b*b-2*a*b exactly in units of 2^-298. Ten 64-bit words
// cover the entire binary32 range plus any physically representable dimension.
class Accumulator {
  std::array<uint64_t,10> words_{};
  void add_word(size_t word,uint64_t value) {
    while(value) {
      if(word==words_.size())throw std::overflow_error("squared norm accumulator overflow");
      uint64_t old=words_[word];words_[word]+=value;value=words_[word]<old;++word;
    }
  }
  void subtract_word(size_t word,uint64_t value) {
    while(value) {
      if(word==words_.size())throw std::logic_error("negative squared norm");
      uint64_t old=words_[word];words_[word]-=value;value=old<value;++word;
    }
  }
  uint64_t bits(size_t start)const {
    size_t word=start/64;unsigned offset=unsigned(start%64);
    uint64_t result=words_.at(word)>>offset;
    if(offset&&word+1<words_.size())result|=words_[word+1]<<(64-offset);
    return result;
  }
  bool lower_nonzero(size_t count)const {
    size_t whole=count/64;unsigned rest=unsigned(count%64);
    for(size_t i=0;i<whole;++i)if(words_[i])return true;
    return rest&&(words_.at(whole)&((uint64_t(1)<<rest)-1));
  }
public:
  void term(uint64_t coefficient,int shift,bool subtract=false) {
    if(!coefficient)return;
    if(shift<0||shift>=int(words_.size()*64))throw std::logic_error("invalid squared norm exponent");
    size_t word=size_t(shift)/64;unsigned offset=unsigned(shift%64);
    if(subtract)subtract_word(word,coefficient<<offset);else add_word(word,coefficient<<offset);
    if(offset) {
      uint64_t high=coefficient>>(64-offset);
      if(subtract)subtract_word(word+1,high);else add_word(word+1,high);
    }
  }
  double rounded()const {
    int high=-1;
    for(int i=int(words_.size())-1;i>=0;--i)if(words_[size_t(i)]) {
      high=i*64+63-int(std::countl_zero(words_[size_t(i)]));break;
    }
    if(high<0)return 0;
    int exponent=high-298;
    uint64_t significand;
    if(high<=52)significand=words_[0]<<(52-high);
    else {
      size_t shift=size_t(high-52);significand=bits(shift)&((uint64_t(1)<<53)-1);
      bool guard=(bits(shift-1)&1)!=0;
      if(guard&&(lower_nonzero(shift-1)||(significand&1)))++significand;
      if(significand==(uint64_t(1)<<53)){significand>>=1;++exponent;}
    }
    // Nonzero binary32 squared norms lie well inside binary64's normal range.
    uint64_t encoded=(uint64_t(exponent+1023)<<52)|(significand&((uint64_t(1)<<52)-1));
    return std::bit_cast<double>(encoded);
  }
};
struct Component {uint32_t mantissa;int exponent;bool negative;};
inline Component component(float value) {
  uint32_t bits=std::bit_cast<uint32_t>(value),exponent=(bits>>23)&255;
  if(exponent==255)throw std::domain_error("non-finite vector component");
  return {(bits&0x7fffff)|(exponent?0x800000u:0u),exponent?int(exponent)-150:-149,(bits>>31)!=0};
}
inline double squared(const float* a,const float* b,int dimension) {
  if(dimension<0)throw std::invalid_argument("negative vector dimension");
  Accumulator sum;
  for(int i=0;i<dimension;++i) {
    auto x=component(a[i]),y=component(b[i]);
    if(a[i]==b[i])continue;
    sum.term(uint64_t(x.mantissa)*x.mantissa,2*x.exponent+298);
    sum.term(uint64_t(y.mantissa)*y.mantissa,2*y.exponent+298);
    sum.term(uint64_t(x.mantissa)*y.mantissa*2,x.exponent+y.exponent+298,x.negative==y.negative);
  }
  return sum.rounded();
}
}
namespace mvd {
// One SQL scalar contract: round the exact stored-float32 squared norm to
// binary64, then take its binary64 square root. Projection and ranking share it.
inline double canonical_l2_distance(const float* a,const float* b,int dimension) {
  return std::sqrt(norm_detail::squared(a,b,dimension));
}
}
