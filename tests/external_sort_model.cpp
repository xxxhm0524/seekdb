#include "external_sort.h"
#include <cassert>
#include <iostream>
#include <random>

struct Row { int64_t id=0;long double number=0;std::string text; };
int main(int argc,char **argv) {
  assert(argc==2);
  std::mt19937_64 random(20261125);
  std::vector<long double> values={0.0L,-0.0L,1.0L,-1.0L,1e-90L,-1e90L,
      std::numeric_limits<long double>::min(),std::numeric_limits<long double>::denorm_min(),
      std::numeric_limits<long double>::max(),std::numeric_limits<long double>::lowest(),
      std::nextafter(1.0L,2.0L),std::nextafter(1.0L,0.0L)};
  int bound=std::min(4000,std::numeric_limits<long double>::max_exponent/2);
  for (size_t i=0;i<10000;++i) {
    long double fraction=std::ldexp(static_cast<long double>(random()),-64);
    fraction+=std::ldexp(static_cast<long double>(random()),-128);
    int exponent=static_cast<int>(random()%static_cast<unsigned>(2*bound+1))-bound;
    long double value=std::ldexp(fraction,exponent);
    values.push_back(i%2?-value:value);
  }
  for (long double value:values) {
    std::string packet;mvd::pack_sort_number(packet,value);
    long double decoded=0;
    assert(packet.size()==mvd::kSortNumberBytes && mvd::unpack_sort_number(packet,decoded));
    assert(value==decoded && std::signbit(value)==std::signbit(decoded));
  }
  std::string bad;mvd::pack_sort_number(bad,1);
  bad[0]=2;long double ignored=0;assert(!mvd::unpack_sort_number(bad,ignored));
  assert(!mvd::unpack_sort_number(std::string_view(bad).substr(1),ignored));
  std::vector<Row> rows;
  for (size_t i=0;i<1000;++i) {
    Row row;row.id=static_cast<int64_t>(i)-500;row.number=values[i%values.size()];
    row.text="中文"+std::string(180,static_cast<char>('a'+i%3))+std::to_string(i%7);
    if (i%11==0) row.text.clear();
    if (i%13==0) {row.text.push_back('\0');row.text+="尾部";}
    if (i==37) row.id=std::numeric_limits<int64_t>::min();
    if (i==38) row.id=std::numeric_limits<int64_t>::max();
    rows.push_back(std::move(row));
  }
  auto encode=[](const Row &row) {
    std::string packet;mvd::pack_sort_number(packet,row.number);
    packet.append(reinterpret_cast<const char *>(&row.id),sizeof(row.id));
    uint32_t size=static_cast<uint32_t>(row.text.size());
    packet.append(reinterpret_cast<const char *>(&size),sizeof(size));packet.append(row.text);return packet;
  };
  auto decode=[](std::string_view packet) {
    Row row;assert(packet.size()>=mvd::kSortNumberBytes+12);
    assert(mvd::unpack_sort_number(packet.substr(0,mvd::kSortNumberBytes),row.number));
    packet.remove_prefix(mvd::kSortNumberBytes);
    std::memcpy(&row.id,packet.data(),sizeof(row.id));packet.remove_prefix(sizeof(row.id));
    uint32_t size=0;std::memcpy(&size,packet.data(),sizeof(size));packet.remove_prefix(sizeof(size));
    assert(size==packet.size());row.text.assign(packet);return row;
  };
  for (bool descending:{false,true}) {
    auto less=[&](const Row &a,const Row &b) {
      if (a.number!=b.number) return descending?a.number>b.number:a.number<b.number;
      if (a.text!=b.text) return a.text<b.text;
      return a.id<b.id;
    };
    auto oracle=rows;std::sort(oracle.begin(),oracle.end(),less);
    for (bool slice:{false,true}) {
      mvd::ExternalSorter<Row> sorter(argv[1],2048,sizeof(Row)+256,4096,less,encode,decode,
          [](const Row &row){return sizeof(Row)+row.text.capacity();});
      for (const auto &row:rows) sorter.push(row);
      size_t first=slice?321:0,count=slice?111:rows.size(),seen=0;
      sorter.finish(first,count,[&](const Row &row) {
        const auto &expected=oracle[first+seen];
        assert(row.id==expected.id && row.number==expected.number && row.text==expected.text);++seen;
      });
      assert(seen==count);
    }
  }
  std::cout << "{\"passed\":true,\"number_roundtrips\":" << values.size()
      << ",\"sorted_rows\":1000,\"multi_run_order_and_slicing\":true,\"signed_ids\":true}\n";
}
