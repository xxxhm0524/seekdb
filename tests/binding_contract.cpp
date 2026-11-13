#include "../src/query/plan.h"
#include "../src/sql.h"
#include "../src/core/error.h"
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
 mvd::TableSchema schema;schema.pk_index=1;schema.columns={{"s",mvd::ColumnType::VARCHAR,8,0},{"pk",mvd::ColumnType::BIGINT,0,0},{"v",mvd::ColumnType::VECTOR,0,2}};mvd::build_row_layout(schema);
 auto bind=[&](const std::string& sql){mvd::SqlParser parser(sql);auto statement=parser.parse();CHECK(statement->kind==mvd::Statement::Kind::SELECT);return mvd::detail::bind_select(schema,*static_cast<mvd::SelectStmt*>(statement.get()));};
 auto alias=bind("SELECT pk AS s FROM t ORDER BY s");CHECK(alias.projections[0].column_index==1&&alias.order[0].column==1);
 auto star=bind("SELECT * FROM t ORDER BY pk DESC,pk ASC LIMIT 0");CHECK(star.projections.size()==3&&star.order.size()==1&&star.order[0].descending);
 auto vector=bind("SELECT pk,L2_DISTANCE(v,'[3,4]') AS d FROM t WHERE pk>=0 ORDER BY d LIMIT 5");CHECK(vector.topk&&vector.limit==5&&vector.vector_column==2&&vector.query==std::vector<float>({3,4}));CHECK(vector.where[0].resolved_column==1&&vector.where[0].typed_value.i==0);
 for(const auto& sql:{"SELECT missing FROM t LIMIT 0","SELECT pk FROM t ORDER BY missing LIMIT 0","SELECT pk FROM t WHERE missing=1 LIMIT 0","SELECT L2_DISTANCE(s,'[0,0]') FROM t LIMIT 0","SELECT pk FROM t WHERE v='[0,0]' LIMIT 0","SELECT pk FROM t ORDER BY L2_DISTANCE(v,'[1]') LIMIT 0"}){
  bool rejected=false;try{bind(sql);}catch(const mvd::DbError&){rejected=true;}CHECK(rejected);
 }
 for(int mode=0;mode<4;++mode){auto bad=schema;if(mode==0)bad.pk_index=100;if(mode==1)bad.columns[0].max_chars=0;if(mode==2)bad.columns[2].dim=0;if(mode==3)bad.columns[0].type=static_cast<mvd::ColumnType>(255);bool rejected=false;try{mvd::build_row_layout(bad);}catch(const mvd::DbError&){rejected=true;}CHECK(rejected);}
 std::cout<<"{\"passed\":true,\"schema_contract\":true,\"alias_precedence\":true,\"bound_plan_owns_values\":true,\"empty_query_still_validated\":true}\n";return 0;
}catch(const mvd::DbError& e){std::cerr<<e.code<<":"<<e.message<<"\n";return 1;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
