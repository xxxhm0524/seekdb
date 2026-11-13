#include "../src/engine.h"
#include <cerrno>
#include <iostream>
#include <random>
static bool fail_sync=false;static int writes=-1;
#ifdef _WIN32
extern "C" int (*__real___imp__commit)(int);
extern "C" int (*__real___imp__write)(int,const void*,unsigned);
static int wrapped_commit(int fd){if(fail_sync){fail_sync=false;errno=ENOSPC;return -1;}return __real___imp__commit(fd);}
static int wrapped_write(int fd,const void* bytes,unsigned length){if(writes==0){writes=-1;errno=ENOSPC;return -1;}if(writes>0)--writes;return __real___imp__write(fd,bytes,length);}
extern "C" {int (*__wrap___imp__commit)(int)=wrapped_commit;int (*__wrap___imp__write)(int,const void*,unsigned)=wrapped_write;}
#else
extern "C" int __real_fsync(int);extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __wrap_fsync(int fd){if(fail_sync){fail_sync=false;errno=ENOSPC;return -1;}return __real_fsync(fd);}
extern "C" ssize_t __wrap_write(int fd,const void* bytes,size_t length){if(writes==0){writes=-1;errno=ENOSPC;return -1;}if(writes>0)--writes;return __real_write(fd,bytes,length);}
#endif
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
static bool contents(mvd::Engine& e,bool full) {
 auto rows=e.execute("SELECT id,s FROM t ORDER BY id");if(!rows.ok||rows.rows.size()!=(full?5u:0u))return false;
 if(!full)return true;
 for(size_t i=0;i<4;++i)if(rows.rows[i][0].i!=int64_t(i+1)||rows.rows[i][1].s!="base"+std::to_string(i+1))return false;
 return rows.rows[4][0].i==9&&rows.rows[4][1].s=="new";
}
static int matrix(const std::string& path) {
 size_t cases=0,violations=0;std::mt19937 random(20261130);
 for(bool dense:{false,true})for(uint64_t cut:{0ull,1ull,65535ull,65536ull,65537ull,131071ull,131072ull})
 for(size_t length:{size_t(1),size_t(32768),size_t(65536),size_t(65537)})for(bool sync_fault:{false,true}) {
  ++cases;mvd::TxSpool spool(path+"/matrix.tmp");std::vector<uint8_t> oracle(3*65536+17),actual;
  for(size_t i=0;i<oracle.size();++i)oracle[i]=dense?uint8_t(random()):uint8_t('a'+i/65536);
  for(size_t offset=0;offset<oracle.size();offset+=65536) {
   auto end=std::min(oracle.size(),offset+65536);spool.append(std::vector<uint8_t>(oracle.begin()+offset,oracle.begin()+end));
  }
  spool.read(cut-cut%65536,65536,actual);bool failed=false;fail_sync=sync_fault;
  try{spool.truncate(cut);}catch(const mvd::DbError&){failed=true;}
  fail_sync=false;bool valid=failed==sync_fault;
  bool outside_rejected=false;try{spool.read(cut,1,actual);}catch(const mvd::DbError&){outside_rejected=true;}
  valid=valid&&outside_rejected;
  std::vector<uint8_t> replacement(length);for(size_t i=0;i<length;++i)replacement[i]=dense?uint8_t(random()):uint8_t('z');
  auto appended=spool.append(replacement);valid=valid&&appended==cut;spool.read(cut,uint32_t(length),actual);valid=valid&&actual==replacement;
  oracle.resize(cut);oracle.insert(oracle.end(),replacement.begin(),replacement.end());
  spool.read(0,uint32_t(oracle.size()),actual);valid=valid&&actual==oracle&&spool.size()==oracle.size();
  for(int n=0;n<8;++n){size_t offset=random()%oracle.size();size_t count=std::min<size_t>(10000,oracle.size()-offset);spool.read(offset,uint32_t(count),actual);valid=valid&&std::equal(actual.begin(),actual.end(),oracle.begin()+offset);}
  if(!valid)++violations;
 }
 std::cout<<"{\"cases\":"<<cases<<",\"violations\":"<<violations<<"}\n";return violations?1:0;
}
int main(int argc,char** argv){try{
 CHECK(argc==3);const std::string mode=argv[2];
 if(mode=="matrix")return matrix(argv[1]);
 if(mode=="verify-full"||mode=="verify-empty"){mvd::Engine e(argv[1]);e.open();CHECK(contents(e,mode=="verify-full"));e.shutdown();return 0;}
 mvd::Engine e(argv[1],16,131100);e.open();CHECK(e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(8184))").ok);
 CHECK(e.execute("BEGIN").ok);CHECK(e.execute("INSERT INTO t VALUES(1,'base1'),(2,'base2'),(3,'base3'),(4,'base4')").ok);
 CHECK(!mvd::path_exists(std::string(argv[1])+"/txn.tmp"));writes=5;fail_sync=true;
 auto failed=e.execute("UPDATE t SET s='changed'");writes=-1;fail_sync=false;CHECK(!failed.ok);
 CHECK(e.execute("INSERT INTO t VALUES(9,'new')").ok);auto found=e.execute("SELECT id,s FROM t WHERE id=9");
 bool valid=found.ok&&found.rows.size()==1&&found.rows[0][0].i==9&&found.rows[0][1].s=="new";
 std::cout<<"{\"passed\":"<<(valid?"true":"false")<<",\"statement_error\":\""<<failed.error_code<<"\",\"rows_for_9\":"<<found.rows.size()<<"}\n";
 CHECK(valid&&contents(e,true));
 if(mode=="commit"||mode=="kill-commit")CHECK(e.execute("COMMIT").ok);
 else if(mode=="rollback")CHECK(e.execute("ROLLBACK").ok);
 if(mode=="kill"||mode=="kill-commit"){std::cout<<"kill-ready\n"<<std::flush;std::cin.get();return 2;}
 e.shutdown();{mvd::Engine check(argv[1]);check.open();CHECK(contents(check,mode=="commit"));check.shutdown();}return 0;
 }catch(const mvd::DbError& ex){std::cerr<<ex.code<<":"<<ex.message<<"\n";return 1;}catch(const std::exception& ex){std::cerr<<ex.what()<<"\n";return 1;}}
