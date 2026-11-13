#include "../src/engine.h"
#include "../src/protocol/jsonl.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(int argc,char** argv){try{
 CHECK(argc==2);mvd::Engine engine(argv[1]);CHECK(!engine.execute("CREATE TABLE t(id BIGINT PRIMARY KEY)").ok);
 engine.open();CHECK(engine.execute("CREATE TABLE t(id BIGINT PRIMARY KEY)").ok);CHECK(engine.execute("INSERT INTO t VALUES(1)").ok);
 CHECK(engine.execute("BEGIN").ok&&engine.execute("INSERT INTO t VALUES(2)").ok);engine.open();CHECK(engine.in_transaction());
 engine.shutdown();CHECK(!engine.execute("SELECT id FROM t").ok);
 for(int i=0;i<3;++i){engine.open();auto rows=engine.execute("SELECT id FROM t ORDER BY id");CHECK(rows.ok&&rows.rows.size()==1&&rows.rows[0][0].i==1);engine.shutdown();}
 std::istringstream input("{\"id\":true,\"op\":\"execute\",\"sql\":\"SELECT id FROM t\"}\n{\"id\":\"bye\",\"op\":\"shutdown\"}\n");std::ostringstream output;volatile std::sig_atomic_t stop=0;
 CHECK(mvd::protocol::serve(engine,input,output,false,stop)==0);
 CHECK(output.str()=="{\"type\":\"ready\"}\n{\"id\":true,\"ok\":true,\"columns\":[\"id\"],\"rows\":[[1]]}\n{\"id\":\"bye\",\"ok\":true}\n");
 std::cout<<"{\"passed\":true,\"closed_database_rejects_execution\":true,\"idempotent_open\":true,\"same_object_reopen\":true,\"protocol_owns_ready_order\":true}\n";return 0;
}catch(const mvd::DbError& e){std::cerr<<e.code<<":"<<e.message<<"\n";return 1;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
