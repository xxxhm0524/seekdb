#include "../src/common.h"
#include "../src/engine.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <omp.h>
static std::atomic<bool> armed{false},injected{false};
void* operator new(std::size_t n) {
  if(omp_in_parallel()&&omp_get_thread_num()==1&&armed.exchange(false)) {injected=true;throw std::bad_alloc();}
  if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(int argc,char** argv) {
 try {
  CHECK(argc==3);omp_set_dynamic(0);omp_set_num_threads(2);
  mvd::Engine engine(argv[1]);engine.open();CHECK(engine.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(128))").ok);
  std::string sql="INSERT INTO t VALUES";
  for(int i=0;i<300;++i) {
    if(i)sql+=",";sql+="("+std::to_string(i)+",'["+std::to_string(i);
    for(int d=1;d<128;++d)sql+=",0";sql+="]')";
  }
  CHECK(engine.execute(sql).ok);
  if(std::string(argv[2])=="indexed")CHECK(engine.execute("CREATE VECTOR INDEX vi ON t(v)").ok);
  std::string query="SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0";for(int d=1;d<128;++d)query+=",0";query+="]') LIMIT 5";
  armed=true;auto failed=engine.execute(query);armed=false;CHECK(injected&&!failed.ok);
  auto recovered=engine.execute(query);CHECK(recovered.ok&&recovered.rows.size()==5);
  for(int i=0;i<5;++i)CHECK(recovered.rows[size_t(i)][0].i==i);
  engine.shutdown();
  std::cout<<"{\"passed\":true,\"worker_exception_returned_as_error\":true,\"subsequent_query_correct\":true}\n";return 0;
 }catch(const mvd::DbError& e){std::cerr<<e.code<<":"<<e.message<<"\n";return 1;}
 catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
