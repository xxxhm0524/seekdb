// Linker-level I/O fault: the real Engine CREATE call must propagate sync failure.
#include "../src/engine.h"
#include <cerrno>
#include <iostream>
#include <cstdlib>
#include <new>
static long fail_allocation_after=-1;
static bool allocation_injected=false;
void* operator new(std::size_t bytes) {
  if(fail_allocation_after==0){fail_allocation_after=-1;allocation_injected=true;throw std::bad_alloc();}
  if(fail_allocation_after>0)--fail_allocation_after;
  if(void* pointer=std::malloc(bytes?bytes:1))return pointer;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* pointer)noexcept{std::free(pointer);}
void operator delete[](void* pointer)noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t)noexcept{std::free(pointer);}
void operator delete[](void* pointer,std::size_t)noexcept{std::free(pointer);}
#ifdef _WIN32
#include <io.h>
extern "C" int (*__real___imp__commit)(int);
static bool fail_next=false;
static int wrapped_commit(int fd) {
  if(fail_next){fail_next=false;errno=ENOSPC;return -1;}
  return __real___imp__commit(fd);
}
extern "C" {int (*__wrap___imp__commit)(int)=wrapped_commit;}
#else
#include <unistd.h>
extern "C" int __real_fsync(int);
extern "C" int __real_rename(const char*,const char*);
static bool fail_rename=false;
extern "C" int __wrap_rename(const char* from,const char* to) {
  if(fail_rename){fail_rename=false;errno=EACCES;return -1;}
  return __real_rename(from,to);
}
static bool fail_next=false;
extern "C" int __wrap_fsync(int fd) {
  if(fail_next){fail_next=false;errno=ENOSPC;return -1;}
  return __real_fsync(fd);
}
#endif
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
static bool catalog_table(const std::string& directory,const std::string& target,bool& index) {
  std::string data;if(!mvd::read_file(directory+"/catalog.dat",data))return false;mvd::Reader r(data);
  uint32_t magic,version,tables;CHECK(r.u32(magic)&&r.u32(version)&&r.u32(tables));
  CHECK(magic==0x4d564443&&version==1);
  for(uint32_t i=0;i<tables;++i) {
    uint32_t id,columns;std::string name;CHECK(r.u32(id)&&r.str(name)&&r.u32(columns));
    for(uint32_t col=0;col<columns;++col){uint8_t type;std::string n;int32_t width,dimension;CHECK(r.u8(type)&&r.str(n)&&r.i32(width)&&r.i32(dimension));}
    int32_t primary,vector;uint8_t indexed;CHECK(r.i32(primary)&&r.i32(vector)&&r.u8(indexed));
    if(name==target){index=indexed!=0;return true;}
  }
  return false;
}
static bool stored_index(const std::string& directory,const std::string& target) {
  bool indexed=false;CHECK(catalog_table(directory,target,indexed));return indexed;
}
int main(int argc,char** argv) {
  try {
    CHECK(argc==3);
    const std::string mode=argv[2];
    if(mode=="replace") {
      const std::string path=std::string(argv[1])+"/atomic-file";
      CHECK(mvd::write_file_atomic(path,"confirmed-old"));
#ifdef _WIN32
      // An open CRT handle denies source rename without changing its bytes.
      auto held=mvd::FileHandle::open(path+".tmp",O_RDWR|O_CREAT);CHECK(held.valid());
#else
      fail_rename=true;
#endif
      bool replaced=mvd::write_file_atomic(path,"replacement");std::string value;
      bool present=mvd::read_file(path,value);
      std::cout<<"{\"replace_ok\":"<<(replaced?"true":"false")<<",\"old_root_preserved\":"<<(present&&value=="confirmed-old"?"true":"false")<<"}\n"<<std::flush;
      CHECK(!replaced&&present&&value=="confirmed-old");return 0;
    }
    if(mode=="allocation") {
      const std::string sql="CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8))";
      size_t injected=0,violations=0;
      for(long point=0;point<90;++point) {
        const std::string directory=std::string(argv[1])+"/case-"+std::to_string(point);
        bool returned_ok=false,visible_before=false,visible_after=false,catalog_after=false;
        {
          mvd::Engine engine(directory,16);engine.open();mvd::Response result;
          allocation_injected=false;fail_allocation_after=point;
          try{result=engine.execute(sql);}catch(const std::exception&){result.ok=false;}
          fail_allocation_after=-1;returned_ok=result.ok;injected+=allocation_injected;
          visible_before=engine.execute("SELECT id FROM t").ok;
          bool ignored=false;catalog_after=catalog_table(directory,"t",ignored);
          auto unrelated=engine.execute("CREATE TABLE other(id BIGINT PRIMARY KEY)");
          if(!unrelated.ok) {
            ++violations;
            std::cout<<"{\"allocation_point\":"<<point<<",\"unrelated_error\":\""<<unrelated.error_code
                     <<"\",\"message\":\""<<unrelated.error_message<<"\"}\n"<<std::flush;
          }
          engine.shutdown();
        }
        {
          mvd::Engine engine(directory,16);engine.open();visible_after=engine.execute("SELECT id FROM t").ok;engine.shutdown();
        }
        if(visible_before!=returned_ok||visible_after!=returned_ok||catalog_after!=returned_ok) {
          ++violations;
          std::cout<<"{\"allocation_point\":"<<point<<",\"create_ok\":"<<(returned_ok?"true":"false")
                   <<",\"visible_after_error\":"<<(visible_before?"true":"false")<<",\"catalog_after_error\":"<<(catalog_after?"true":"false")<<",\"visible_after_restart\":"<<(visible_after?"true":"false")<<"}\n";
        }
      }
      std::cout<<"{\"allocation_cases\":90,\"actual_injections\":"<<injected<<",\"violations\":"<<violations<<"}\n"<<std::flush;
      CHECK(injected>10&&violations==0);return 0;
    }
    if(mode=="index"||mode=="existing-index") {
      const bool existing=mode=="existing-index";
      bool before=false,after=false,index_ok=true;
      {
        mvd::Engine engine(argv[1],16);engine.open();
        CHECK(engine.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2))").ok);
        CHECK(engine.execute("INSERT INTO t VALUES(1,'[3,4]')").ok);
        if(existing)CHECK(engine.execute("CREATE VECTOR INDEX original ON t(v)").ok);
        before=stored_index(argv[1],"t");
        fail_next=true;auto response=engine.execute("CREATE VECTOR INDEX vi ON t(v)");index_ok=response.ok;CHECK(!fail_next);
        CHECK(engine.execute("CREATE TABLE other(id BIGINT PRIMARY KEY)").ok);after=stored_index(argv[1],"t");
        engine.set_ann_mode(true);
        auto selected=engine.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 1");
        CHECK(selected.ok&&selected.rows.size()==1&&selected.rows[0][0].i==1&&selected.rows[0][1].d==5);engine.shutdown();
      }
      std::cout<<"{\"index_ok\":"<<(index_ok?"true":"false")<<",\"indexed_before\":"<<(before?"true":"false")
               <<",\"indexed_after_unrelated_ddl\":"<<(after?"true":"false")<<"}\n"<<std::flush;
      CHECK(!index_ok&&before==existing&&after==existing);
      std::cout<<"INDEX sync failure regression PASS\n";return 0;
    }
    if(mode=="drop") {
      bool before_visible=false,after_visible=false,drop_ok=true;
      {
        mvd::Engine engine(argv[1],16);engine.open();
        CHECK(engine.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8))").ok);
        CHECK(engine.execute("INSERT INTO t VALUES(1,'ok')").ok);
        fail_next=true;auto result=engine.execute("DROP TABLE t");drop_ok=result.ok;
        CHECK(!fail_next);
        auto selected=engine.execute("SELECT id,s FROM t");
        before_visible=selected.ok&&selected.rows.size()==1&&selected.rows[0][0].i==1&&selected.rows[0][1].s=="ok";
        engine.shutdown();
      }
      {
        mvd::Engine engine(argv[1],16);engine.open();auto selected=engine.execute("SELECT id,s FROM t");
        after_visible=selected.ok&&selected.rows.size()==1&&selected.rows[0][0].i==1&&selected.rows[0][1].s=="ok";engine.shutdown();
      }
      std::cout<<"{\"drop_ok\":"<<(drop_ok?"true":"false")<<",\"visible_after_error\":"<<(before_visible?"true":"false")
               <<",\"visible_after_restart\":"<<(after_visible?"true":"false")<<"}\n"<<std::flush;
      CHECK(!drop_ok&&before_visible&&after_visible);
      std::cout<<"DROP sync failure regression PASS\n";return 0;
    }
    CHECK(mode=="create");
    {
      mvd::Engine engine(argv[1],16);engine.open();
      fail_next=true;
      auto result=engine.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8))");
      std::cout<<"{\"injected_sync_consumed\":"<<(!fail_next?"true":"false")
               <<",\"create_ok\":"<<(result.ok?"true":"false")<<"}\n"<<std::flush;
      CHECK(!fail_next);CHECK(!result.ok);CHECK(result.error_code=="IO_ERROR");
      CHECK(!engine.execute("SELECT id FROM t").ok);
      CHECK(engine.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8))").ok);
      CHECK(engine.execute("INSERT INTO t VALUES(1,'ok')").ok);engine.shutdown();
    }
    {
      mvd::Engine engine(argv[1],16);engine.open();
      auto result=engine.execute("SELECT id,s FROM t");
      CHECK(result.ok&&result.rows.size()==1&&result.rows[0][0].i==1&&result.rows[0][1].s=="ok");engine.shutdown();
    }
    std::cout<<"CREATE sync failure regression PASS\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
