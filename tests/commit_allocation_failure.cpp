#include "../src/common.h"
#include "../src/engine.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <utility>
#include <cerrno>
static bool fail_sync=false;
#ifdef _WIN32
extern "C" int (*__real___imp__commit)(int);
static int wrapped_commit(int fd) {
  if(fail_sync){fail_sync=false;errno=ENOSPC;return -1;}
  return __real___imp__commit(fd);
}
extern "C" {int (*__wrap___imp__commit)(int)=wrapped_commit;}
#else
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if(fail_sync){fail_sync=false;errno=ENOSPC;return -1;}
  return __real_fsync(fd);
}
#endif
static long remaining=-1;static bool injected=false;
void* operator new(std::size_t n){if(remaining==0){remaining=-1;injected=true;throw std::bad_alloc();}if(remaining>0)--remaining;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
static std::string state(mvd::Engine& e) {
  std::string s;
  for(const char* name:{"t","u"}) {
    auto r=e.execute(std::string("SELECT id,s FROM ")+name+" ORDER BY id");
    CHECK(r.ok);s+=name;
    for(auto& row:r.rows)s+=std::to_string(row[0].i)+":"+row[1].s+";";
    for(int id=1;id<=3;++id) {
      auto point=e.execute(std::string("SELECT id,s FROM ")+name+" WHERE id="+std::to_string(id));CHECK(point.ok);
      s+="|"+std::to_string(id)+"=";
      for(auto& row:point.rows)s+=std::to_string(row[0].i)+":"+row[1].s+";";
    }
    auto ann=e.execute(std::string("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM ")+name+" ORDER BY d LIMIT 3");
    CHECK(ann.ok);s+="|ann=";
    for(auto& row:ann.rows)s+=std::to_string(row[0].i)+";";
  }
  return s;
}
int main(int argc,char** argv) {
 try {
  CHECK(argc==3);
  std::string mode=argv[2];
  bool cross_page=mode.rfind("cross-page-",0)==0;
  if(cross_page)mode.erase(0,11);
  bool wide=cross_page||mode=="wide"||mode=="shutdown"||mode=="retry";
  bool sync_fault=mode=="sync-live"||mode=="sync-shutdown";
  bool early_shutdown=mode=="shutdown"||mode=="retry"||mode=="sync-shutdown";
  long limit=sync_fault?1:180;
  size_t violations=0,injections=0;
  for(long point=0;point<limit;++point) {
    const std::string directory=std::string(argv[1])+"/case-"+std::to_string(point);
    std::string old_state,new_state,live_state,restart_state;bool ok=false;
    {
      mvd::Engine e(directory,wide?1:16,300,0);e.open();e.set_ann_mode(true);
      for(const char* name:{"t","u"}) {
        CHECK(e.execute(std::string("CREATE TABLE ")+name+(cross_page?"(id BIGINT PRIMARY KEY,s VARCHAR(25000),v VECTOR(2))":wide?"(id BIGINT PRIMARY KEY,s VARCHAR(8000),v VECTOR(2))":"(id BIGINT PRIMARY KEY,s VARCHAR(32),v VECTOR(2))")).ok);
        CHECK(e.execute(std::string("INSERT INTO ")+name+" VALUES(1,'base','[3,4]'),(2,'remove','[0,1]')").ok);
        CHECK(e.execute(std::string("CREATE VECTOR INDEX idx ON ")+name+"(v)").ok);
      }
      old_state=state(e);CHECK(e.execute("BEGIN").ok);
      for(const char* name:{"t","u"}) {
        CHECK(e.execute(std::string("UPDATE ")+name+" SET s='new' WHERE id=1").ok);
        CHECK(e.execute(std::string("DELETE FROM ")+name+" WHERE id=2").ok);
        CHECK(e.execute(std::string("INSERT INTO ")+name+" VALUES(3,'added','[1,0]')").ok);
      }
      new_state=state(e);
      remaining=sync_fault?-1:point;injected=false;fail_sync=sync_fault;
      try{ok=e.execute("COMMIT").ok;}catch(...){ok=false;}
      remaining=-1;injections+=sync_fault?!fail_sync:injected;fail_sync=false;
      if(sync_fault)CHECK(!ok);
      if(!ok&&mode=="retry") {
        remaining=0;injected=false;bool read_ok=true;
        try{read_ok=e.execute("SELECT id FROM t").ok;}catch(...){read_ok=false;}
        remaining=-1;CHECK(!read_ok&&injected);
      }
      if(!early_shutdown) {CHECK(e.execute("ROLLBACK").ok);live_state=state(e);}
      e.shutdown();
    }
    {mvd::Engine e(directory,wide?1:16,300,0);e.open();e.set_ann_mode(true);restart_state=state(e);e.shutdown();}
    bool valid=(early_shutdown||live_state==restart_state)&&
      (restart_state==new_state||(!ok&&restart_state==old_state));
    if(!valid){++violations;std::cout<<"point="<<point<<" ok="<<ok<<" old="<<old_state<<" new="<<new_state<<" live="<<live_state<<" restart="<<restart_state<<"\n"<<std::flush;}
  }
  std::cout<<"{\"cases\":"<<limit<<",\"actual_injections\":"<<injections<<",\"violations\":"<<violations<<"}\n";return violations?1:0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
 catch(const mvd::DbError& e){std::cerr<<e.code<<":"<<e.message<<"\n";return 1;}
}
