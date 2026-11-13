#pragma once
#include "../model/result.h"
#include <csignal>
#include <istream>
#include <ostream>
namespace mvd {class Engine; namespace protocol {
struct Request {std::string id_raw="null",op,sql,search_mode;};
bool parse_request(const std::string&,Request&,std::string&);
std::string encode_response(const Response&,const std::string&);
std::string encode_error(const std::string&,const std::string&,const std::string&);
int serve(Engine&,std::istream&,std::ostream&,bool,const volatile std::sig_atomic_t&);
}}
