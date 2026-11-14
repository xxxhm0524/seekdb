// Request parser and bounded string-prefix scan adapted directly from
// juanzhi-gcb/seekdb dfee55b84284f4c39d2df45951cdbcb1d1baf1a2,
// src/protocol/jsonl.hpp. Source hash is frozen in tests/reference-successful-version.json.
#include "request.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif
namespace mvd::wire {
// Preserve raw validated JSON identifiers, avoiding any conversion through double.
// Raw lexemes borrow the immutable input frame until the response is finished.
using Value = mvd::RequestValue;
inline std::size_t plain_string_prefix_portable(std::string_view input) noexcept {
    constexpr std::uint64_t ones=0x0101010101010101ULL,high=0x8080808080808080ULL;
    const auto zero=[](std::uint64_t x){return (x-ones)&~x&high;};
    std::size_t size=0;
    while(input.size()-size>=8) {
        std::uint64_t word;std::memcpy(&word,input.data()+size,8);
        // The subtraction tests can stop early on a neighbouring byte after
        // a borrow, but never skip a quote, backslash or raw control byte.
        if(zero(word^(ones*'"')) || zero(word^(ones*'\\')) ||
           ((word-ones*32)&~word&high))break;
        size+=8;
    }
    while(size<input.size()) {
        const auto c=static_cast<unsigned char>(input[size]);
        if(c<32 || c=='"' || c=='\\')break;
        ++size;
    }
    return size;
}
#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx2"),noinline))
inline std::size_t plain_string_prefix_avx2(std::string_view input) noexcept {
    const auto quote=_mm256_set1_epi8('"'),slash=_mm256_set1_epi8('\\');
    const auto top=_mm256_set1_epi8(static_cast<char>(0xe0)),zero=_mm256_setzero_si256();
    std::size_t size=0;
    while(input.size()-size>=32){
        const auto bytes=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(input.data()+size));
        const auto control=_mm256_cmpeq_epi8(_mm256_and_si256(bytes,top),zero);
        const auto special=_mm256_or_si256(control,_mm256_or_si256(_mm256_cmpeq_epi8(bytes,quote),_mm256_cmpeq_epi8(bytes,slash)));
        const auto mask=static_cast<unsigned>(_mm256_movemask_epi8(special));
        if(mask)return size+std::countr_zero(mask);
        size+=32;
    }
    return size+plain_string_prefix_portable(input.substr(size));
}
#endif
inline std::size_t plain_string_prefix(std::string_view input) noexcept {
#if defined(__x86_64__) && defined(__GNUC__)
    static const bool available=__builtin_cpu_supports("avx2");
    if(input.size()>=64 && available)return plain_string_prefix_avx2(input);
#endif
    return plain_string_prefix_portable(input);
}
class Parser {
    std::string_view input;
    std::size_t pos = 0;
    [[noreturn]] void bad() const { throw std::invalid_argument("invalid JSON request"); }
    void space() { while (pos < input.size() && (input[pos]==' ' || input[pos]=='\t' || input[pos]=='\r' || input[pos]=='\n')) ++pos; }
    bool take(char c) { space(); if(pos<input.size() && input[pos]==c) { ++pos; return true; } return false; }
    unsigned hex() {
        unsigned n=0;
        for(int i=0;i<4;++i) {
            if(pos==input.size()) bad();
            const char c=input[pos++]; unsigned v;
            if(c>='0'&&c<='9') v=unsigned(c-'0');
            else if(c>='a'&&c<='f') v=unsigned(c-'a'+10);
            else if(c>='A'&&c<='F') v=unsigned(c-'A'+10);
            else bad();
            n=n*16+v;
        }
        return n;
    }
    void utf8(std::string& s,unsigned n) {
        if(n<0x80) s+=char(n);
        else if(n<0x800) {s+=char(0xc0|(n>>6));s+=char(0x80|(n&63));}
        else if(n<0x10000) {s+=char(0xe0|(n>>12));s+=char(0x80|((n>>6)&63));s+=char(0x80|(n&63));}
        else {s+=char(0xf0|(n>>18));s+=char(0x80|((n>>12)&63));s+=char(0x80|((n>>6)&63));s+=char(0x80|(n&63));}
    }
    std::string string() {
        if(!take('"')) bad();
        std::string s;
        while(pos<input.size()) {
            const auto count=plain_string_prefix(input.substr(pos));
            s.append(input.substr(pos,count));pos+=count;
            if(pos==input.size())bad();
            unsigned char c=static_cast<unsigned char>(input[pos++]);
            if(c=='"') { mvd::utf8_length(s); return s; }
            if(c<32) bad();
            if(c!='\\') { s+=char(c); continue; }
            if(pos==input.size()) bad();
            switch(input[pos++]) {
            case '"': s+='"';break; case '\\':s+='\\';break;case '/':s+='/';break;
            case 'b':s+='\b';break;case 'f':s+='\f';break;case 'n':s+='\n';break;case 'r':s+='\r';break;case 't':s+='\t';break;
            case 'u': {
                unsigned n=hex();
                if(n>=0xd800 && n<=0xdbff) {
                    if(pos+2>input.size() || input[pos]!='\\' || input[pos+1]!='u') bad();
                    pos+=2; unsigned low=hex();
                    if(low<0xdc00 || low>0xdfff) bad();
                    n=0x10000+((n-0xd800)<<10)+(low-0xdc00);
                } else if(n>=0xdc00 && n<=0xdfff) bad();
                utf8(s,n); break;
            }
            default: bad();
            }
        }
        bad();
    }
    Value scalar() {
        space(); const auto start=pos;
        if(pos==input.size()) bad();
        Value v; v.kind=input[pos];
        if(v.kind=='"') v.text=string();
        else if(v.kind=='t'||v.kind=='f'||v.kind=='n') {
            const std::string_view literal=v.kind=='t'?"true":v.kind=='f'?"false":"null";
            if(input.substr(pos,literal.size())!=literal) bad();
            pos+=literal.size();
        } else {
            if(input[pos]=='-') ++pos;
            if(pos==input.size()) bad();
            auto digit=[&] { return pos<input.size() && input[pos]>='0' && input[pos]<='9'; };
            if(input[pos]=='0') ++pos;
            else { if(!digit()) bad(); while(digit()) ++pos; }
            if(pos<input.size() && input[pos]=='.') { ++pos;if(!digit()) bad();while(digit()) ++pos; }
            if(pos<input.size() && (input[pos]=='e'||input[pos]=='E')) {
                ++pos;if(pos<input.size() && (input[pos]=='+'||input[pos]=='-')) ++pos;
                if(!digit()) bad();
                while(digit()) ++pos;
            }
            v.kind='0';
        }
        v.raw=input.substr(start,pos-start);
        // Integer identifiers are arbitrary precision lexemes. JSON floating IDs
        // must remain finite for the official Python driver (C locale is unchanged).
        if(v.kind=='0' && v.raw.find_first_of(".eE")!=std::string::npos &&
           !std::isfinite(std::strtod(std::string(v.raw).c_str(),nullptr))) bad();
        return v;
    }
    Value value() {
        space();
        if(pos==input.size()) bad();
        if(input[pos]!='{' && input[pos]!='[') return scalar();
        const auto start=pos;
        Value result; result.kind=input[pos];
        // An explicit stack validates opaque container values without a fixed
        // nesting limit or recursion on the C++ call stack. Nested objects still
        // reject duplicate decoded keys before any SQL may execute.
        enum State { ArrayFirst, ArrayValue, ArrayNext, ObjectFirst, ObjectKey, ObjectNext };
        struct Frame { State state; std::set<std::string> keys; };
        std::vector<Frame> frames;
        auto atom=[&] {
            space(); if(pos==input.size()) bad();
            if(input[pos]=='[') { ++pos; frames.push_back({ArrayFirst,{}}); }
            else if(input[pos]=='{') { ++pos; frames.push_back({ObjectFirst,{}}); }
            else scalar();
        };
        atom();
        while(!frames.empty()) {
            const auto state=frames.back().state;
            if(state==ArrayFirst || state==ArrayValue) {
                if(state==ArrayFirst && take(']')) { frames.pop_back(); continue; }
                frames.back().state=ArrayNext; atom();
            } else if(state==ArrayNext) {
                if(take(']')) frames.pop_back();
                else { if(!take(',')) bad(); frames.back().state=ArrayValue; }
            } else if(state==ObjectFirst || state==ObjectKey) {
                if(state==ObjectFirst && take('}')) { frames.pop_back(); continue; }
                auto key=string();
                if(!frames.back().keys.insert(std::move(key)).second || !take(':')) bad();
                frames.back().state=ObjectNext; atom();
            } else {
                if(take('}')) frames.pop_back();
                else { if(!take(',')) bad(); frames.back().state=ObjectKey; }
            }
        }
        result.raw=input.substr(start,pos-start);
        return result;
    }
    std::map<std::string,Value> object() {
        if(!take('{')) bad();
        std::map<std::string,Value> result;
        if(!take('}')) {
            do {
                auto key=string(); if(!take(':')) bad();
                auto v=value();
                if(!result.emplace(std::move(key),std::move(v)).second) bad();
            } while(take(','));
            if(!take('}')) bad();
        }
        return result;
    }
public:
    explicit Parser(std::string_view s):input(s) {}
    auto parse() { auto result=object();space();if(pos!=input.size()) bad();return result; }
};
}
namespace mvd {
RequestFields parse_request_fields(const std::string& line) {
    if (line.find('\0') != std::string::npos) fail("invalid_request", "raw NUL in JSON request");
    std::string_view input(line);
    if (input.size() >= 3 && static_cast<unsigned char>(input[0]) == 0xef &&
        static_cast<unsigned char>(input[1]) == 0xbb && static_cast<unsigned char>(input[2]) == 0xbf) input.remove_prefix(3);
    try { return wire::Parser(input).parse(); }
    catch (const Error&) { fail("invalid_request", "invalid UTF-8 in JSON request"); }
    catch (const std::invalid_argument&) { fail("invalid_request", "malformed JSON request"); }
}
}
