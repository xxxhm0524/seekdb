#include "model.hpp"
#include <iostream>
#include <random>
using namespace mvd;
int main(){ size_t checked=0; auto check=[&](const std::string& s){
 std::string expected="prefix", actual="prefix"; bool a=false,b=false;
 try { expected+=Json(s).dump(); } catch(...) {a=true;}
 try { append_value_json(actual,Value(s)); } catch(...) {b=true;}
 if(a!=b || actual!=expected) throw std::runtime_error("JSON string bytes or rejection differ");
 if(!a && Json::parse(actual.substr(6)).get<std::string>()!=s) throw std::runtime_error("JSON string round trip");++checked;};
 for(size_t n: std::vector<size_t>{0,1,7,8,9,15,16,17,31,32,33,63,64,65,127,128,129,1024}){
  std::string s(n,'a');check(s);
  for(size_t at=0;at<n;++at)for(unsigned c=0;c<256;++c){s[at]=char(c);check(s);s[at]='a';}
 }
 std::mt19937_64 rng(113573);for(size_t n=0;n<5000;++n){std::string s(rng()%200,'a');for(auto& c:s)c=char(rng()%256);check(s);}
 for(const auto& x: {std::string("\xc3\xa9"),std::string("\xe4\xb8\xad"),std::string("\xf0\x9f\x98\x80"),std::string("\xc0\x80"),std::string("\xed\xa0\x80")})for(size_t at=0;at<65;++at)check(std::string(at,'a')+x+std::string(65-at,'b'));
 std::cout<<"{\"passed\":true,\"string_checks\":"<<checked<<"}\n"; }
