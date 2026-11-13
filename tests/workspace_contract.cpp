#include "../src/transaction/workspace.h"
#include "../src/storage/store.h"
#include <cerrno>
#include <iostream>
#include <stdexcept>
static int writes=-1;static bool fail_sync=false;
#ifdef _WIN32
extern "C" int (*__real___imp__commit)(int);
extern "C" int (*__real___imp__write)(int,const void*,unsigned);
static int sync_call(int fd){if(fail_sync){fail_sync=false;errno=ENOSPC;return -1;}return __real___imp__commit(fd);}
static int write_call(int fd,const void* bytes,unsigned length){if(writes==0){writes=-1;errno=ENOSPC;return -1;}if(writes>0)--writes;return __real___imp__write(fd,bytes,length);}
extern "C" {int (*__wrap___imp__commit)(int)=sync_call;int (*__wrap___imp__write)(int,const void*,unsigned)=write_call;}
#else
extern "C" int __real_fsync(int);extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __wrap_fsync(int fd){if(fail_sync){fail_sync=false;errno=ENOSPC;return -1;}return __real_fsync(fd);}
extern "C" ssize_t __wrap_write(int fd,const void* bytes,size_t length){if(writes==0){writes=-1;errno=ENOSPC;return -1;}if(writes>0)--writes;return __real_write(fd,bytes,length);}
#endif
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
static_assert(!std::is_copy_constructible_v<mvd::TransactionWorkspace>);
static_assert(std::is_const_v<typename decltype(mvd::RowState{}.spool)::element_type>);
int main(int argc,char** argv){try{
 CHECK(argc==2);mvd::TableSchema schema;schema.id=1;schema.name="t";schema.pk_index=0;
 schema.columns={{"id",mvd::ColumnType::BIGINT,0,0},{"s",mvd::ColumnType::VARCHAR,8184,0}};mvd::build_row_layout(schema);CHECK(schema.row_size==32768);
 mvd::PageCache cache(16);mvd::Table table(schema,std::string(argv[1])+"/unused",&cache);
 mvd::TransactionWorkspace tx(std::string(argv[1])+"/txn.tmp",131100,[](uint32_t,const mvd::RowState& state,std::vector<uint8_t>& bytes){state.read_raw(bytes);for(const auto& patch:state.patches)for(const auto& change:*patch)std::fill(bytes.begin(),bytes.end(),uint8_t(change.second.s[0]));});
 auto noop=[]{};
 auto accounting=[&]{size_t actual=0;for(const auto& entry:tx.tables())for(const auto& row:entry.second.rows)actual+=row.second.row.size();CHECK(tx.resident_bytes()==actual&&actual<=131100);};
 auto insert=[&](int64_t pk,char value){mvd::RowState row;row.pos=tx.allocate_position(table);row.row.assign(schema.row_size,uint8_t(value));tx.stage(table,pk,std::move(row));};
 auto check=[&](int64_t pk,char value){auto row=tx.find_row(1,pk);CHECK(row&&!row->deleted);std::vector<uint8_t> actual;row->read_raw(actual);CHECK(actual==std::vector<uint8_t>(schema.row_size,uint8_t(value)));};
 auto update=[&](int64_t pk,const std::shared_ptr<const mvd::RowPatch>& patch){mvd::RowState row=*tx.find_row(1,pk);row.patches.push_back(patch);tx.stage(table,pk,std::move(row));};
 tx.begin();tx.execute_statement([&]{for(int i=1;i<=4;++i)insert(i,char('a'+i));return 4;},noop);CHECK(!tx.spilled());accounting();
 auto patch=tx.track_patch({{1,mvd::Value::make_string("X")}});writes=5;fail_sync=true;bool rejected=false;
 try{tx.execute_statement([&]{for(int i=1;i<=4;++i)update(i,patch);return 4;},noop);}catch(const mvd::DbError&){rejected=true;}
 writes=-1;fail_sync=false;CHECK(rejected&&tx.active()&&tx.spilled());accounting();CHECK(tx.resident_bytes()==32768);
 tx.execute_statement([&]{insert(9,'Z');return 1;},noop);check(9,'Z');for(int i=1;i<=4;++i)check(i,char('a'+i));accounting();
 auto second=tx.track_patch({{1,mvd::Value::make_string("Y")}});tx.execute_statement([&]{update(1,second);return 1;},noop);check(1,'Y');accounting();CHECK(tx.resident_bytes()==0);
 bool failed=false;try{tx.execute_statement([&]()->int{insert(10,'Q');throw std::runtime_error("statement error");},noop);}catch(const std::runtime_error&){failed=true;}
 CHECK(failed&&!tx.find_row(1,10));check(1,'Y');check(9,'Z');accounting();
 failed=false;try{tx.execute_statement([&]()->int{return tx.execute_statement([]{return 0;},noop);},noop);}catch(const mvd::DbError&){failed=true;}
 CHECK(failed);tx.execute_statement([&]{insert(11,'R');return 1;},noop);CHECK(tx.find_row(1,11)->pos==5);accounting();
 tx.clear();CHECK(!tx.active()&&tx.tables().empty()&&tx.resident_bytes()==0&&!mvd::path_exists(std::string(argv[1])+"/txn.tmp"));
 int commits=0;tx.execute_statement([&]{insert(20,'P');return 1;},[&]{++commits;check(20,'P');});CHECK(commits==1&&!tx.active()&&tx.tables().empty());
 std::cout<<"{\"passed\":true,\"savepoint_and_payload_coherence\":true,\"accounting_after_io_error\":true,\"nested_scope_rejected\":true,\"autocommit_owned_by_workspace\":true}\n";return 0;
}catch(const mvd::DbError& e){std::cerr<<e.code<<":"<<e.message<<"\n";return 1;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
