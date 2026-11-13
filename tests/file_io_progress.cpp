#include "../src/common.h"
#include <iostream>
static int mode=0,calls=0;
static bool zero_write(){if(mode!=1)return false;++calls;errno=ENOSPC;return true;}
#ifdef _WIN32
extern "C" int (*__real___imp__write)(int,const void*,unsigned);
extern "C" __int64 __real__lseeki64(int,__int64,int);
static int wrapped_write(int fd,const void* data,unsigned length) {
  if(zero_write())return calls<=8?0:-1;
  return __real___imp__write(fd,data,length);
}
extern "C" {int (*__wrap___imp__write)(int,const void*,unsigned)=wrapped_write;}
extern "C" __int64 __wrap__lseeki64(int fd,__int64 offset,int whence) {
  if(mode==2){++calls;errno=EIO;return -1;}return __real__lseeki64(fd,offset,whence);
}
#else
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_write(int fd,const void* data,size_t length){if(zero_write())return calls<=8?0:-1;return __real_write(fd,data,length);}
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t length,off_t offset){if(zero_write())return calls<=8?0:-1;return __real_pwrite(fd,data,length,offset);}
#endif
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(int argc,char** argv) {
 try {
  CHECK(argc==2);int violations=0,checks=0;
  const std::string path=std::string(argv[1])+"/file";
  auto file=mvd::FileHandle::open(path,O_RDWR|O_CREAT|O_TRUNC);CHECK(file.valid());
  for(bool append:{false,true}) {
    mode=1;calls=0;bool ok=append?file.append("x",1):file.pwrite_full("x",1,0);mode=0;++checks;
    bool protected_state=!ok&&calls==1;if(!protected_state)++violations;
    std::cout<<"{\"append\":"<<append<<",\"write_calls\":"<<calls<<",\"protected_state\":"<<protected_state<<"}\n";
  }
#ifdef _WIN32
  for(bool write:{false,true}) {
    CHECK(file.ftruncate(0)&&file.pwrite_full("abcdef",6,0));CHECK(__real__lseeki64(file.fd(),0,SEEK_SET)==0);
    char value=0;mode=2;calls=0;
    bool ok=write?file.pwrite_full("Z",1,3):file.pread_full(&value,1,3);mode=0;++checks;
    char actual[6]{};CHECK(file.pread_full(actual,6,0));
    bool protected_state=!ok&&calls==1&&std::memcmp(actual,"abcdef",6)==0;
    if(!protected_state)++violations;
    std::cout<<"{\"seek_write\":"<<write<<",\"returned_ok\":"<<ok<<",\"protected_state\":"<<protected_state<<"}\n";
  }
#endif
  std::cout<<"{\"checks\":"<<checks<<",\"violations\":"<<violations<<"}\n";return violations?1:0;
 }catch(const std::exception& e){mode=0;std::cerr<<e.what()<<"\n";return 1;}
}
