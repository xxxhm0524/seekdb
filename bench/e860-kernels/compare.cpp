#include "model.hpp"
#include "control.hpp"
#include "candidate.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <sched.h>

using Control=mvd::ControlIntegerScreen;
using Candidate=mvd::CandidateIntegerScreen;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}

int main() try {
    cpu_set_t allowed,one;CPU_ZERO(&allowed);require(sched_getaffinity(0,sizeof(allowed),&allowed)==0,"get affinity");int cpu=-1;
    for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&allowed)){cpu=i;break;}
    require(cpu>=0,"no CPU");CPU_ZERO(&one);CPU_SET(cpu,&one);require(sched_setaffinity(0,sizeof(one),&one)==0,"set affinity");
    std::vector<std::pair<std::string,unsigned>> kernels;
#ifdef MVD_BATCH_X86
    __builtin_cpu_init();
    if(__builtin_cpu_supports("avx2"))kernels.push_back({"avx2",2});
    if(__builtin_cpu_supports("avx512f")&&__builtin_cpu_supports("avx512vnni"))kernels.push_back({"vnni",3});
#endif
    require(!kernels.empty(),"no SIMD backend");
    std::mt19937 rng(2026111619);std::uniform_real_distribution<float> number(-1,1);mvd::Json records=mvd::Json::array();size_t comparisons=0;
    for(size_t dimensions:{33u,128u,129u,768u}){
        const size_t count=dimensions==768?1027:10003,query_count=100;
        std::vector<float> rows(count*dimensions);for(float& v:rows)v=number(rng);
        // Include extreme codes, zero and padded dimensions/last blocks.
        rows[0]=-1;rows[1]=1;rows[2]=-0.0f;
        auto read=[&](size_t r,size_t d){return rows[r*dimensions+d];};
        auto control=Control::create(count,dimensions,64*1024*1024,read);auto candidate=Candidate::create(count,dimensions,64*1024*1024,read);
        require(bool(control)&&bool(candidate),"cache refused");
        std::vector<std::vector<float>> values(query_count,std::vector<float>(dimensions));for(auto& q:values)for(float& v:q)v=number(rng);
        std::vector<Control::Query> cq;std::vector<Candidate::Query> nq;
        for(const auto& v:values){cq.push_back(control->query(v));nq.push_back(candidate->query(v));}
        for(auto [name,implementation]:kernels){
            for(size_t q=0;q<query_count;++q)for(size_t first=0;first<count;first+=32){
                std::array<int32_t,32> a{},b{},scalar{};auto am=control->scores(cq[q],first,a,implementation);auto bm=candidate->scores(nq[q],first,b,implementation);
                auto sm=candidate->scores(nq[q],first,scalar,1);
                require(a==b&&b==scalar&&am==bm&&bm==sm&&bm==*std::min_element(b.begin(),b.end()),"scores/minimum differ");++comparisons;
            }
            auto timed=[&](auto& index,const auto& queries){
                uint64_t checksum=0;const auto start=std::chrono::steady_clock::now();
                for(unsigned repeat=0;repeat<5;++repeat)for(const auto& q:queries)for(size_t first=0;first<count;first+=32){
                    std::array<int32_t,32> output;const auto minimum=index->scores(q,first,output,implementation);
                    checksum+=uint32_t(minimum);checksum+=uint32_t(output[(repeat+first)%32]);
                }
                const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
                return std::pair<int64_t,uint64_t>{ns,checksum};
            };
            timed(control,cq);timed(candidate,nq);
            for(unsigned round=0;round<6;++round){
                std::pair<int64_t,uint64_t> a,b;
                if(round%2){b=timed(candidate,nq);a=timed(control,cq);}else{a=timed(control,cq);b=timed(candidate,nq);}
                require(a.second==b.second,"timed checksum mismatch");
                mvd::Json row={{"kernel",name},{"dimensions",dimensions},{"rows",count},{"round",round+1},{"control_ns",a.first},{"candidate_ns",b.first},{"speedup",double(a.first)/double(b.first)},{"checksum",a.second}};
                records.push_back(row);std::cout<<row.dump()<<'\n'<<std::flush;
            }
        }
    }
    std::cout<<mvd::Json({{"passed",true},{"cpu",cpu},{"comparisons",comparisons},{"records",records},{"scope","Exact integer kernels only; same process, fixed CPU, AB/BA order, no formal E or whole-database gain inferred."}}).dump()<<'\n';
} catch(const std::exception& error){std::cout<<mvd::Json({{"passed",false},{"error",error.what()}}).dump()<<'\n';return 1;}
