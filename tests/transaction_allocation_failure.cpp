#include "../src/common.h"
#if __has_include("../src/storage/store.h")
#include "../src/storage/store.h"
#endif
#include "../src/engine.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <utility>
static long remaining=-1;static bool injected=false;
void* operator new(std::size_t n){if(remaining==0){remaining=-1;injected=true;throw std::bad_alloc();}if(remaining>0)--remaining;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
using Expected=std::vector<std::pair<int64_t,std::string>>;
static void cache_failures(const std::string& directory) {
  size_t count=0;
  for(long point=0;point<80;++point) {
    mvd::PageCache cache(2);
    mvd::TableSchema schema;schema.id=1;schema.name="cache";schema.pk_index=0;
    schema.columns.push_back({"id",mvd::ColumnType::BIGINT,0,0});mvd::build_row_layout(schema);
    mvd::Table table(schema,directory+"/cache-"+std::to_string(point),&cache);table.open();
    auto first=mvd::make_page();first->page_no=0;first->payload[0]=17;
    cache.install(&table,0,first);
    // A hit only relinks a node, so even the next allocation being unavailable
    // must not invalidate the cache's existing page or allocate a list node.
    remaining=0;injected=false;bool hit=false;
    try { hit=cache.get(&table,0)==first; } catch(...) {}
    remaining=-1;CHECK(hit&&!injected);
    first.reset();
    auto second=mvd::make_page();second->page_no=1;second->payload[0]=29;
    remaining=point;injected=false;
    try {cache.install(&table,1,second);}catch(const std::bad_alloc&){}
    remaining=-1;count+=injected;
    cache.install(&table,1,second);second.reset();
    // Eviction and reinsertion exercise map/list coherence after a failed
    // admission, including failed node/bucket allocation.
    for(uint32_t page_no=2;page_no<10;++page_no) {
      auto page=mvd::make_page();page->page_no=page_no;cache.install(&table,page_no,page);
    }
    CHECK(cache.get(&table,0)->payload[0]==17);
    CHECK(cache.get(&table,1)->payload[0]==29);
    cache.drop_table(1);CHECK(cache.size()==0);
  }
  std::cout<<"{\"cases\":80,\"actual_injections\":"<<count<<",\"violations\":0}\n";
}
static bool snapshot(mvd::Engine& e,const Expected& expected) {
  auto r=e.execute("SELECT id,s,v FROM t ORDER BY id");if(!r.ok||r.rows.size()!=expected.size())return false;
  for(size_t i=0;i<expected.size();++i) {
    if(r.rows[i][0].i!=expected[i].first||r.rows[i][1].s!=expected[i].second)return false;
    std::vector<float> v=expected[i].first==1?std::vector<float>{3,4}:expected[i].first==2?std::vector<float>{1,0}:expected[i].first==3?std::vector<float>{0,1}:std::vector<float>{0,0};
    if(r.rows[i][2].vec!=v)return false;
  }
  return true;
}
int main(int argc,char** argv) {
 try {
  if(argc==3&&std::string(argv[2])=="cache"){cache_failures(argv[1]);return 0;}
  CHECK(argc==2||argc==4);size_t failures=0,total_injected=0,cases=0;
  const std::string commands[]={"INSERT INTO t VALUES(2,'x','[1,0]'),(3,'y','[0,1]')","UPDATE t SET s='new'","DELETE FROM t WHERE id>=0"};
  for(int operation=0;operation<3;++operation)for(long point=0;point<160;++point) {
    if(argc==4&&(operation!=std::atoi(argv[2])||point!=std::atol(argv[3])))continue;
    ++cases;
    std::string directory=std::string(argv[1])+"/case-"+std::to_string(operation)+"-"+std::to_string(point);
    Expected expected={{0,"prior"},{1,"base"}};if(operation)expected.push_back({4,"prior2"});
    bool ok=false,live=false,committed=false,restarted=false;
    {
      mvd::Engine engine(directory,16,300);engine.open();
      CHECK(engine.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(32),v VECTOR(2))").ok);
      CHECK(engine.execute("INSERT INTO t VALUES(1,'base','[3,4]')").ok);CHECK(engine.execute("BEGIN").ok);
      CHECK(engine.execute("INSERT INTO t VALUES(0,'prior','[0,0]')").ok);
      if(operation)CHECK(engine.execute("INSERT INTO t VALUES(4,'prior2','[0,0]')").ok);
      injected=false;remaining=point;mvd::Response response;
      try{response=engine.execute(commands[operation]);}catch(...){response.ok=false;}
      remaining=-1;total_injected+=injected;ok=response.ok;
      if(ok) {
        if(operation==0){expected.push_back({2,"x"});expected.push_back({3,"y"});}
        else if(operation==1){for(auto& row:expected)row.second="new";}
        else expected.clear();
      }
      live=engine.in_transaction()&&snapshot(engine,expected);
      committed=engine.execute("COMMIT").ok;engine.shutdown();
    }
    {mvd::Engine engine(directory,16,300);engine.open();restarted=snapshot(engine,expected);engine.shutdown();}
    if(!live||!committed||!restarted){++failures;std::cout<<"{\"operation\":"<<operation<<",\"point\":"<<point<<",\"statement_ok\":"<<ok<<",\"live\":"<<live<<",\"committed\":"<<committed<<",\"restarted\":"<<restarted<<"}\n"<<std::flush;}
  }
  std::cout<<"{\"cases\":"<<cases<<",\"actual_injections\":"<<total_injected<<",\"violations\":"<<failures<<"}\n";
  return failures?1:0;
 }catch(const mvd::DbError& e){std::cerr<<e.code<<": "<<e.message<<'\n';return 1;}
 catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

