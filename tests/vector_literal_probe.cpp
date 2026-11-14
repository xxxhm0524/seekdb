#include "../src/vector_literal.hpp"
#include "../src/model.hpp"
#include <bit>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>

static std::vector<float> baseline(const std::string& text,uint32_t width) {
    auto data=mvd::Json::parse(text);
    if(!data.is_array()||data.size()!=width)throw std::runtime_error("shape");
    std::vector<float> out;out.reserve(width);
    for(const auto&x:data){
        if(!x.is_number())throw std::runtime_error("type");
        double d=x.get<double>();double threshold=std::ldexp(2.0-std::ldexp(1.0,-24),127),maximum=std::numeric_limits<float>::max();
        if(!std::isfinite(d)||std::abs(d)>=threshold)throw std::runtime_error("overflow");
        out.push_back(d>maximum?std::numeric_limits<float>::max():d<-maximum?-std::numeric_limits<float>::max():static_cast<float>(d));
    }
    return out;
}
static void check(const std::string& text,uint32_t width) {
    bool old_ok=true,new_ok=true;std::vector<float> old_value,new_value;
    try{old_value=baseline(text,width);}catch(const std::exception&){old_ok=false;}
    try{new_value=mvd::parse_vector_literal(text,width);}catch(const std::exception&){new_ok=false;}
    if(old_ok!=new_ok)throw std::runtime_error("literal acceptance mismatch: "+mvd::Json(text).dump());
    if(old_ok){
        if(old_value.size()!=new_value.size())throw std::runtime_error("literal shape mismatch");
        for(size_t i=0;i<old_value.size();++i)if(old_value[i]!=new_value[i] && std::bit_cast<uint32_t>(old_value[i])!=std::bit_cast<uint32_t>(new_value[i]))
            throw std::runtime_error("literal float32 mismatch");
    }
}
int main() try {
    size_t checked=0;
    for(const std::string&number:{"0","-0","1","-1","1.5","1e3","1E+3","1e-3","1e-400","-1e-400","3.4028235e38","-3.4028235e38","3.4028235677973366e38","1e400","00","01","+1",".5","1.","NaN","Infinity","true","null","\"1\"","1e","1e-","-","--1","0x1","[]","{}","1 2"}){
        check("["+number+",0]",2);++checked;
    }
    for(const std::string&text:{"[]","[1]","[1,2,3]","[1,]","[,1]","[1,2]extra"," [1,2]\n","[1, 2]","[1,\n2]"}){check(text,2);++checked;}
    for(const std::string& text:{std::string("\xef\xbb\xbf")+"[1,2]",std::string("[1,\xc2\xa0 2]")}){check(text,2);++checked;}
    // Strict JSON requires complete input. The old JSON library treats raw NUL
    // as an end marker; retain that observation and explicitly reject the tail.
    const std::string nul_tail("[1,2]\0tail",10);
    bool legacy_accepted=true,nul_rejected=false;
    try{baseline(nul_tail,2);}catch(const std::exception&){legacy_accepted=false;}
    try{mvd::parse_vector_literal(nul_tail,2);}catch(const mvd::Error&){nul_rejected=true;}
    if(!legacy_accepted||!nul_rejected)throw std::runtime_error("strict NUL-tail rejection failed");++checked;
    check("[0]",UINT32_MAX);++checked;
    check("[0]",0);++checked;
    std::mt19937 rng(81293);std::vector<std::string> corpus;
    for(int trial=0;trial<1200;++trial){
        mvd::Json values=mvd::Json::array();uint32_t width=trial%3==0?128:3;
        for(uint32_t d=0;d<width;++d){float value=std::bit_cast<float>(uint32_t(rng())&0xfeffffffU);values.push_back(double(value));}
        auto text=values.dump();check(text,width);++checked;
        if(width==128&&corpus.size()<100)corpus.push_back(text);
    }
    const std::string alphabet="012eE-+.,[] \t\ntrue\"{}";
    for(int trial=0;trial<16000;++trial){
        std::string text;
        for(size_t i=0,n=rng()%18;i<n;++i)text+=alphabet[rng()%alphabet.size()];
        check(text,1+(rng()%3));++checked;
    }
    double old_checksum=0,new_checksum=0;auto start=std::chrono::steady_clock::now();
    for(int i=0;i<10000;++i){auto v=baseline(corpus[i%corpus.size()],128);old_checksum+=v[0];}
    double old_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();start=std::chrono::steady_clock::now();
    for(int i=0;i<10000;++i){auto v=mvd::parse_vector_literal(corpus[i%corpus.size()],128);new_checksum+=v[0];}
    double new_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    if(old_checksum!=new_checksum)throw std::runtime_error("parse checksum mismatch");
    std::cout<<std::setprecision(17)<<"{\"passed\":true,\"parity_cases\":"<<checked<<",\"nul_tail_hardened\":true,\"json_baseline_seconds\":"<<old_seconds<<",\"direct_parser_seconds\":"<<new_seconds<<"}\n";
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
