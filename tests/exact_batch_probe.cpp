#include "exact_batch_proto.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

static double point(const float* vector,const float* query,size_t dimensions) {
    double sum=0;
    for(size_t d=0;d<dimensions;++d){double delta=double(vector[d])-double(query[d]);sum+=delta*delta;}
    return std::sqrt(sum);
}
int main() {
    std::vector<mvd::batch::Kernel> kernels{{"scalar",mvd::batch::scalar}};
#ifdef MVD_BATCH_X86
    __builtin_cpu_init();
    if(__builtin_cpu_supports("avx2"))kernels.push_back({"avx2",mvd::batch::avx2});
    if(__builtin_cpu_supports("avx512f")&&__builtin_cpu_supports("avx2"))kernels.push_back({"avx512",mvd::batch::avx512});
#endif
    std::mt19937 rng(91371);size_t checked=0;
    for(size_t dimensions:{1u,2u,3u,7u,33u,128u})for(int trial=0;trial<32;++trial){
        std::vector<float> block(dimensions*8),query(dimensions),row(dimensions);
        for(float&x:block)x=std::bit_cast<float>(uint32_t(rng())&0xfeffffffU);
        for(float&x:query)x=trial%3?std::bit_cast<float>(uint32_t(rng())&0xfeffffffU):0.0f;
        block[0]=std::numeric_limits<float>::max();block[1]=-block[0];
        block[2]=std::numeric_limits<float>::denorm_min();block[3]=-0.0f;
        for(const auto&kernel:kernels){
            std::array<double,8> output{};kernel.evaluate(block.data(),query.data(),dimensions,output.data());
            for(size_t lane=0;lane<8;++lane){
                for(size_t d=0;d<dimensions;++d)row[d]=block[d*8+lane];
                double expected=point(row.data(),query.data(),dimensions);
                if(std::bit_cast<uint64_t>(expected)!=std::bit_cast<uint64_t>(output[lane]))throw std::runtime_error("batch distance bit mismatch");
                ++checked;
            }
        }
    }
    constexpr size_t count=10000,dimensions=128,queries=200;
    std::uniform_real_distribution<float> distribution(-1,1);
    std::vector<float> rows(count*dimensions),blocked(count*dimensions),q(queries*dimensions);
    for(float&x:rows)x=distribution(rng);for(float&x:q)x=distribution(rng);
    for(size_t i=0;i<count;++i)for(size_t d=0;d<dimensions;++d)blocked[(i/8)*dimensions*8+d*8+i%8]=rows[i*dimensions+d];
    auto start=std::chrono::steady_clock::now();double checksum=0;
    for(size_t k=0;k<queries;++k)for(size_t i=0;i<count;++i)checksum+=point(rows.data()+i*dimensions,q.data()+k*dimensions,dimensions);
    double scalar_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout<<std::setprecision(17)<<"{\"passed\":true,\"bit_checks\":"<<checked<<",\"aos_scalar_seconds\":"<<scalar_seconds<<",\"kernels\":[";
    bool first=true;
    for(const auto&kernel:kernels){
        start=std::chrono::steady_clock::now();double result=0;
        for(size_t k=0;k<queries;++k)for(size_t i=0;i<count;i+=8){
            std::array<double,8> output{};kernel.evaluate(blocked.data()+(i/8)*dimensions*8,q.data()+k*dimensions,dimensions,output.data());
            for(double value:output)result+=value;
        }
        double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        if(std::bit_cast<uint64_t>(result)!=std::bit_cast<uint64_t>(checksum))throw std::runtime_error("timed checksum mismatch");
        if(!first)std::cout<<',';first=false;
        std::cout<<"{\"name\":\""<<kernel.name<<"\",\"seconds\":"<<seconds<<",\"checksum\":"<<result<<'}';
    }
    std::cout<<"]}\n";
}
