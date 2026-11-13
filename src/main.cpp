#include "engine.h"
#include "model/layout.h"
#include "app/options.h"
#include "protocol/jsonl.h"
#include <csignal>
#include <cstdio>
#include <iostream>
#include <algorithm>
#ifdef _OPENMP
#include <omp.h>
#endif
namespace {volatile std::sig_atomic_t stop=0;void handle_signal(int){stop=1;}}
int main(int argc,char** argv){
 try{
  auto opt=mvd::app::parse_options(argc,argv);
#ifdef _OPENMP
  omp_set_num_threads(opt.query_threads);
#endif
  std::signal(SIGINT,handle_signal);std::signal(SIGTERM,handle_signal);
  size_t pages=std::max<size_t>(8,size_t(opt.cache_mb)*1024*1024/mvd::kPageSize);
  mvd::Engine engine(opt.data_dir,pages,size_t(opt.tx_memory_mb)*1024*1024,size_t(opt.ann_min_rows),size_t(opt.sort_memory_mb)*1024*1024);
  return mvd::protocol::serve(engine,std::cin,std::cout,opt.ann_default,stop);
 }catch(const mvd::DbError& e){std::fprintf(stderr,"fatal: %s: %s\n",e.code.c_str(),e.message.c_str());return 1;}
 catch(const std::exception& e){std::fprintf(stderr,"fatal: %s\n",e.what());return 1;}
}
