#include "../src/exact_l2.h"
#include <iostream>
#include <vector>
int main() {
  int n;
  while(std::cin>>n) {
    if(n<0||n>16384)return 2;
    std::vector<float> a(size_t(n),0),b(size_t(n),0);
    for(float& value:a){uint32_t bits;if(!(std::cin>>bits))return 2;value=std::bit_cast<float>(bits);}
    for(float& value:b){uint32_t bits;if(!(std::cin>>bits))return 2;value=std::bit_cast<float>(bits);}
    auto squared=mvd::norm_detail::squared(a.data(),b.data(),n);
    auto distance=mvd::canonical_l2_distance(a.data(),b.data(),n);
    std::cout<<std::hex<<std::bit_cast<uint64_t>(squared)<<' '<<std::bit_cast<uint64_t>(distance)<<'\n';
  }
}
