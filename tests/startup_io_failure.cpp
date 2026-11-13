#include "../src/engine.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
static std::array<int,4096> files{};
static int fault=0,remaining=0,injections=0;
static bool fail(int fd,int operation) {
  if(fd<0||fd>=int(files.size()))return false;
  int kind=files[size_t(fd)];
  bool match=(fault==2&&kind==1&&operation==2)||(fault==3&&kind==1&&operation==3)||
      (fault==4&&kind==2&&operation==3)||(fault==5&&kind==2&&operation==2);
  if(!match||remaining<=0)return false;
  if(fault==5&&remaining>1){--remaining;return false;}
  --remaining;++injections;errno=EIO;return true;
}
static int opened(const char* path,int fd) {
  if(fd>=0&&fd<int(files.size()))files[size_t(fd)]=std::strstr(path,"/catalog.dat")?1:std::strstr(path,"/t_1.dat")?2:0;
  return fd;
}
#ifdef _WIN32
extern "C" int (*__real___imp__open)(const char*,int,...);
extern "C" int (*__real___imp__read)(int,void*,unsigned);
extern "C" __int64 __real__lseeki64(int,__int64,int);
static int wrapped_open(const char* path,int flags,...) {
  if(fault==1&&remaining&&std::strstr(path,"/catalog.dat")){--remaining;++injections;errno=EIO;return -1;}
  return opened(path,__real___imp__open(path,flags,_S_IREAD|_S_IWRITE));
}
static int wrapped_read(int fd,void* data,unsigned length){return fail(fd,3)?-1:__real___imp__read(fd,data,length);}
extern "C" __int64 __wrap__lseeki64(int fd,__int64 offset,int whence){return whence==SEEK_END&&fail(fd,2)?-1:__real__lseeki64(fd,offset,whence);}
extern "C" {
int (*__wrap___imp__open)(const char*,int,...)=wrapped_open;
int (*__wrap___imp__read)(int,void*,unsigned)=wrapped_read;
}
#else
extern "C" int __real_open(const char*,int,...);
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" off_t __real_lseek(int,off_t,int);
extern "C" int __wrap_open(const char* path,int flags,...) {
  if(fault==1&&remaining&&std::strstr(path,"/catalog.dat")){--remaining;++injections;errno=EIO;return -1;}
  return opened(path,__real_open(path,flags,0644));
}
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t length,off_t offset){return fail(fd,3)?-1:__real_pread(fd,data,length,offset);}
extern "C" off_t __wrap_lseek(int fd,off_t offset,int whence){return whence==SEEK_END&&fail(fd,2)?-1:__real_lseek(fd,offset,whence);}
#endif
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
static bool intact(mvd::Engine& engine,bool added=false) {
  auto r=engine.execute("SELECT id,s FROM t ORDER BY id");
  return r.ok&&r.rows.size()==(added?3u:2u)&&r.rows[0][0].i==1&&r.rows[0][1].s=="first"&&r.rows[1][0].i==2&&r.rows[1][1].s=="last"&&(!added||(r.rows[2][0].i==3&&r.rows[2][1].s=="new"));
}
int main(int argc,char** argv) {
 try {
  CHECK(argc==2);int violations=0,total=0;
  for(int mode=1;mode<=5;++mode) {
    const std::string directory=std::string(argv[1])+"/case-"+std::to_string(mode);
    {mvd::Engine e(directory);e.open();CHECK(e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8000))").ok);
     CHECK(e.execute("INSERT INTO t VALUES(1,'first'),(2,'last')").ok);e.shutdown();}
    bool ready=false,protected_state=true,added=false;
    {
      mvd::Engine e(directory);fault=mode;remaining=mode==5?2:1;
#ifndef _WIN32
      if(mode==4)remaining=2; // O_DIRECT read and its buffered fallback.
#endif
      injections=0;
      try{e.open();ready=true;}catch(const mvd::DbError&){}
      fault=0;remaining=0;CHECK(injections>0);total+=injections;
      if(ready)protected_state=intact(e);
      if(ready&&mode==5){added=e.execute("INSERT INTO t VALUES(3,'new')").ok;protected_state=added&&intact(e,true);}
      e.shutdown();
    }
    {mvd::Engine e(directory);e.open();protected_state=protected_state&&intact(e,added);e.shutdown();}
    if(!protected_state)++violations;
    std::cout<<"{\"mode\":"<<mode<<",\"reported_ready\":"<<ready<<",\"protected_state\":"<<protected_state<<"}\n";
  }
  std::cout<<"{\"cases\":5,\"actual_injections\":"<<total<<",\"violations\":"<<violations<<"}\n";
  return violations?1:0;
 }catch(const mvd::DbError& e){std::cerr<<e.code<<":"<<e.message<<"\n";return 1;}
 catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
