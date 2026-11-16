#include "model.hpp"
#include "quantized_screen.hpp"
#include "fixtures/error-control.hpp"
#include <chrono>
#include <bit>
#include <random>
#include <iostream>
using New=mvd::QuantizedScreen;using Old=mvd::ControlQuantizedScreen;
static void need(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
int main()try{
    std::mt19937_64 rng(2026111622);size_t successor_checks=0,bound_checks=0;
    auto check=[](double value){double a=mvd::rounding::positive_successor(value),b=std::nextafter(value,std::numeric_limits<double>::infinity());need(std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b),"successor differs");};
    for(uint64_t bits:{uint64_t(0),uint64_t(1),UINT64_C(0xfffffffffffff),UINT64_C(0x10000000000000),UINT64_C(0x3ff0000000000000),UINT64_C(0x7fefffffffffffff)}){check(std::bit_cast<double>(bits));++successor_checks;}
    for(unsigned i=0;i<200000;++i){uint64_t bits=rng()&UINT64_C(0x7fffffffffffffff);if(bits>=UINT64_C(0x7ff0000000000000))bits=0;check(std::bit_cast<double>(bits));++successor_checks;}
    for(size_t dims:{32u,33u,128u,129u,1024u,4096u})for(unsigned trial=0;trial<8;++trial){
        constexpr size_t count=65;std::vector<float> rows(count*dims),query(dims);
        for(float&v:rows)v=trial%3?std::bit_cast<float>(uint32_t(rng())&UINT32_C(0xfeffffff)):float(int(rng()%100)-50)/32;
        for(float&v:query)v=trial%3?std::bit_cast<float>(uint32_t(rng())&UINT32_C(0xfeffffff)):float(int(rng()%100)-50)/32;
        if(trial==0)std::fill(query.begin(),query.end(),0.0f);rows[0]=std::numeric_limits<float>::max();rows[1]=-rows[0];rows[2]=std::numeric_limits<float>::denorm_min();
        auto read=[&](size_t r,size_t d){return rows[r*dims+d];};auto a=New::create(count,dims,32<<20,read);auto b=Old::create(count,dims,32<<20,read);need(bool(a)==bool(b),"construction");if(!a)continue;
        auto aq=a->query(query);auto bq=b->query(query);need(aq.codes==bq.codes&&aq.norm==bq.norm&&aq.sum==bq.sum&&std::bit_cast<uint64_t>(aq.error)==std::bit_cast<uint64_t>(bq.error),"query envelope differs");
        for(size_t first=0;first<count;first+=32){std::array<int32_t,32>x{},y{};auto xm=a->scores(aq,first,x);auto ym=b->scores(bq,first,y);need(x==y&&xm==ym,"integer scores differ");for(int32_t score:x){need(std::bit_cast<uint64_t>(a->lower(aq,score))==std::bit_cast<uint64_t>(b->lower(bq,score)),"lower envelope differs");++bound_checks;}}
    }
    constexpr size_t dims=128,count=10000;std::vector<float> rows(dims*count);for(float&v:rows)v=float(int(rng()%2049)-1024)/1024;auto read=[&](size_t r,size_t d){return rows[r*dims+d];};mvd::Json samples=mvd::Json::array();
    for(unsigned round=0;round<6;++round){int64_t old_ns=0,new_ns=0;auto run=[&](bool old){auto start=std::chrono::steady_clock::now();if(old){auto index=Old::create(count,dims,32<<20,read);need(bool(index),"old prep");}else{auto index=New::create(count,dims,32<<20,read);need(bool(index),"new prep");}return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();};if(round%2){new_ns=run(false);old_ns=run(true);}else{old_ns=run(true);new_ns=run(false);}samples.push_back({{"old_ns",old_ns},{"new_ns",new_ns},{"speedup",double(old_ns)/new_ns}});}
    std::cout<<mvd::Json({{"passed",true},{"successor_checks",successor_checks},{"bound_bit_checks",bound_checks},{"samples",samples},{"scope","Numerical values match nextafter exactly; no FP status-flag equivalence or formal score is claimed."}}).dump()<<'\n';
}catch(const std::exception&e){std::cout<<mvd::Json({{"passed",false},{"error",e.what()}}).dump()<<'\n';return 1;}
