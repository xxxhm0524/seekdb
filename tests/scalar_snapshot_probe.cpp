#include "vector_index.hpp"
#include <bit>
#include <iostream>
#include <map>
#include <numeric>
#include <random>

using namespace mvd;
static void need(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static int cmp(const Value& a,const Value& b){
    if(auto x=std::get_if<int64_t>(&a)){auto y=std::get<int64_t>(b);return(*x>y)-(*x<y);}
    const auto& x=std::get<std::string>(a);const auto& y=std::get<std::string>(b);return(x>y)-(x<y);
}
int main(int argc,char** argv)try{
    need(argc==2,"output directory required");std::filesystem::path root=argv[1];need(!std::filesystem::exists(root),"existing output");std::filesystem::create_directories(root);
    std::mt19937 random(2026111609);size_t cells=0,comparisons=0,searches=0,bounds=0;
    for(unsigned text_mode=0;text_mode<3;++text_mode)for(bool mapping:{false,true})for(unsigned layout=0;layout<3;++layout){
        ::setenv("MVD_READ_MAP_MIB",mapping?"64":"0",1);::setenv("MVD_VECTOR_CACHE_MIB","2",1);
        auto directory=root/(std::to_string(text_mode)+"-"+std::to_string(mapping)+"-"+std::to_string(layout));std::filesystem::create_directory(directory);Storage store(directory);
        Table table;table.name="t";std::vector<Column> definitions={{"unused",Type::Text,8192},{"v",Type::Vector,129},{"g",Type::Integer,0},{"id",Type::Integer,0},{"s",Type::Text,2048},{"n",Type::Integer,0}};
        std::rotate(definitions.begin(),definitions.begin()+layout,definitions.end());table.columns=definitions;
        const size_t primary=table.column("id"),vec=table.column("v"),group=table.column("g"),text=table.column("s"),ordinary=table.column("n"),unused=table.column("unused");table.primary=primary;
        std::vector<bool> wanted(6,true);wanted[unused]=false;std::map<int64_t,Row> truth;
        std::vector<size_t> order(641);std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),random);BTree tree(store);
        for(size_t k:order){
            int64_t key=k==0?INT64_MIN:k==1?INT64_MAX:int64_t(k)-321;Row row(6);row[primary]=key;row[group]=int64_t(k%7)-3;row[ordinary]=k%2?INT64_MIN:int64_t(k);
            row[text]=text_mode==1?std::string("unique")+std::to_string(k):text_mode==2&&k%2?std::string(1024,'q')+std::to_string(k):k%3==0?std::string("n\0x",3):k%3==1?std::string("\xe4\xb8\xad\xf0\x9f\x98\x80"):std::string();row[unused]=std::string(2048,'x');
            std::vector<float> v(129);for(size_t d=0;d<v.size();++d)v[d]=k<3?-0.0f:float(int((k*17+d*3)%101)-50)/16;row[vec]=std::move(v);truth[key]=row;
            Ref ref=store.append(encode_row(table,row));tree.put(table.root,key,ref);++table.count;table.live_bytes+=ref.length;
        }
        store.commit({table});store.prepare_read_mapping();size_t clustered=layout==0?group:layout==1?text:primary;
        auto index=VectorIndex::create(store,table,wanted,clustered);need(bool(index),"cache declined");need(index->entries().size()==truth.size(),"row count");
        for(size_t i=0;i<index->entries().size();++i){
            const auto& expected=truth.at(index->entries()[i].key);Row actual=index->materialize(i);
            for(size_t col=0;col<6;++col)if(wanted[col]){
                if(col==vec){const auto&a=std::get<std::vector<float>>(actual[col]);const auto&b=std::get<std::vector<float>>(expected[col]);for(size_t d=0;d<a.size();++d)need(std::bit_cast<uint32_t>(a[d])==std::bit_cast<uint32_t>(b[d]),"float bits");}
                else{
                    need(actual[col]==expected[col],"scalar materialization");std::string a,b;index->append_scalar_json(a,i,col);append_value_json(b,expected[col]);need(a==b,"scalar JSON");
                    const std::vector<Value> probes=table.columns[col].type==Type::Integer?std::vector<Value>{int64_t(INT64_MIN),int64_t(-1),int64_t(0),int64_t(INT64_MAX)}:std::vector<Value>{std::string(),std::string("n\0x",3),std::string("zz"),std::string("\xe4\xb8\xad\xf0\x9f\x98\x80")};
                    for(const auto& v:probes){need(index->compare(i,col,v)==cmp(expected[col],v),"comparison");++comparisons;}
                }++cells;
            }
        }
        const std::vector<Value> values=table.columns[clustered].type==Type::Integer?std::vector<Value>{int64_t(INT64_MIN),int64_t(0),int64_t(1),int64_t(INT64_MAX)}:std::vector<Value>{std::string(),std::string("n\0x",3),std::string("zz")};
        for(const auto& v:values){size_t lo=0,hi=0;for(const auto&[key,row]:truth){lo+=cmp(row[clustered],v)<0;hi+=cmp(row[clustered],v)<=0;}auto range=index->equal_bounds(clustered,v);need(range&&*range==std::pair<size_t,size_t>{lo,hi},"bounds");++bounds;}
        for(unsigned filter=0;filter<4;++filter)for(size_t k:{1u,10u,17u,700u}){
            std::vector<float> query(129,0.125f);std::vector<std::pair<int64_t,double>> expected;
            auto accepts=[&](int64_t key,const Row& row){return filter==0||(filter==1&&std::get<int64_t>(row[group])>=1)||(filter==2&&std::get<std::string>(row[text])!=std::string("n\0x",3))||(filter==3&&key<0);};
            for(const auto&[key,row]:truth)if(accepts(key,row)){double sum=0;const auto&v=std::get<std::vector<float>>(row[vec]);for(size_t d=0;d<v.size();++d){double delta=double(v[d])-double(query[d]);sum+=delta*delta;}expected.push_back({key,std::sqrt(sum)});}
            std::sort(expected.begin(),expected.end(),[](auto&a,auto&b){return a.second==b.second?a.first<b.first:a.second<b.second;});if(expected.size()>k)expected.resize(k);
            auto predicate=[&](size_t i){return filter==0||(filter==1&&index->compare(i,group,Value(int64_t(1)))>=0)||(filter==2&&index->compare(i,text,Value(std::string("n\0x",3)))!=0)||(filter==3&&index->compare(i,primary,Value(int64_t(0)))<0);};
            auto hits=index->search(query,k,predicate);need(hits.size()==expected.size(),"search count");for(size_t j=0;j<hits.size();++j)need(index->entries()[hits[j].entry].key==expected[j].first&&std::bit_cast<uint64_t>(hits[j].distance)==std::bit_cast<uint64_t>(expected[j].second),"oracle distance/key");++searches;
        }
    }
    std::cout<<Json({{"passed",true},{"scalar_and_vector_cells",cells},{"scalar_comparisons",comparisons},{"bound_queries",bounds},{"oracle_searches",searches},{"layouts",3},{"mapping_modes",2},{"text_distributions",3}}).dump()<<'\n';
}catch(const std::exception& e){std::cout<<Json({{"passed",false},{"error",e.what()}}).dump()<<'\n';return 1;}
