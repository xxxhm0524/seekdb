// Query-local bounded sort runs. The scratch file is never used by recovery.
#pragma once
#include "../common.h"
#include <functional>
#include <queue>

namespace mvd {
inline constexpr size_t kSortNumberWords=(std::numeric_limits<long double>::digits+31)/32;
inline constexpr size_t kSortNumberBytes=1+sizeof(int32_t)+kSortNumberWords*sizeof(uint32_t);
inline void pack_sort_number(std::string &packet,long double value) {
  if (!std::isfinite(value)) throw_error("CORRUPTION","nonfinite query sort key");
  uint8_t negative=std::signbit(value)?1:0;
  int exponent=0;
  long double fraction=std::frexp(std::fabs(value),&exponent);
  int32_t stored_exponent=exponent;
  packet.append(reinterpret_cast<const char *>(&negative),sizeof(negative));
  packet.append(reinterpret_cast<const char *>(&stored_exponent),sizeof(stored_exponent));
  for (size_t i=0;i<kSortNumberWords;++i) {
    fraction=std::ldexp(fraction,32);
    uint32_t part=static_cast<uint32_t>(fraction);
    packet.append(reinterpret_cast<const char *>(&part),sizeof(part));
    fraction-=part;
  }
}
inline bool unpack_sort_number(std::string_view packet,long double &value) {
  if (packet.size()!=kSortNumberBytes) return false;
  uint8_t negative=static_cast<uint8_t>(packet[0]);
  int32_t exponent=0;std::memcpy(&exponent,packet.data()+1,sizeof(exponent));
  if (negative>1 || exponent>std::numeric_limits<long double>::max_exponent ||
      exponent<std::numeric_limits<long double>::min_exponent-std::numeric_limits<long double>::digits) return false;
  long double mantissa=0;
  for (size_t i=0;i<kSortNumberWords;++i) {
    uint32_t part=0;
    std::memcpy(&part,packet.data()+1+sizeof(exponent)+i*sizeof(part),sizeof(part));
    mantissa=std::ldexp(mantissa,32)+part;
  }
  value=std::ldexp(mantissa,exponent-static_cast<int>(32*kSortNumberWords));
  if (negative) value=-value;
  return std::isfinite(value);
}

template<class Row> class ExternalSorter {
 public:
  using Less=std::function<bool(const Row &,const Row &)>;
  using Encode=std::function<std::string(const Row &)>;
  using Decode=std::function<Row(std::string_view)>;
  using Memory=std::function<size_t(const Row &)>;
  ExternalSorter(std::string path,size_t budget,size_t minimum_row,uint32_t maximum_record,
                 Less less,Encode encode,Decode decode,Memory memory)
      :scratch_(std::move(path)),budget_(budget),maximum_record_(maximum_record),
       less_(std::move(less)),encode_(std::move(encode)),decode_(std::move(decode)),memory_(std::move(memory)) {
    buffer_.reserve(std::max<size_t>(1,budget_/std::max(minimum_row,sizeof(Row))));
  }
  ExternalSorter(const ExternalSorter &)=delete;
  ExternalSorter &operator=(const ExternalSorter &)=delete;
  void push(Row row) {
    size_t charge=memory_(row);
    size_t extra=charge>sizeof(Row)?charge-sizeof(Row):0;
    size_t base=buffer_.capacity()*sizeof(Row);
    size_t room=budget_>base?budget_-base:0;
    if (!buffer_.empty() && (buffer_.size()==buffer_.capacity() || extra>room || extra_>room-extra)) flush();
    buffer_.push_back(std::move(row));
    extra_+=extra;
    if (buffer_.size()==buffer_.capacity() || extra_>=room) flush();
  }
  void finish(uint64_t skip,uint64_t take,const std::function<void(const Row &)> &emit) {
    flush();
    std::vector<Run> runs;
    for (auto &level:levels_) for (auto run:level) runs.push_back(run);
    while (runs.size()>kFanIn) {
      std::vector<Run> next;
      for (size_t first=0;first<runs.size();first+=kFanIn) {
        size_t last=std::min(runs.size(),first+kFanIn);
        if (last-first==1) next.push_back(runs[first]);
        else next.push_back(merge_run(std::vector<Run>(runs.begin()+first,runs.begin()+last)));
      }
      runs=std::move(next);
    }
    merge(runs,skip,take,emit);
  }
 private:
  static constexpr size_t kFanIn=16;
  static constexpr size_t kBufferBytes=65536;
  struct Scratch {
    std::string path;
    FileHandle file;
    explicit Scratch(std::string name):path(std::move(name)),file(FileHandle::open(path,O_RDWR|O_CREAT|O_TRUNC)) {
      if (!file.valid()) throw_error("IO_ERROR","cannot open query sort file");
#ifndef _WIN32
      if (std::remove(path.c_str())!=0) throw_error("IO_ERROR","cannot unlink query sort file");
#endif
    }
    ~Scratch() {
      file.close();
#ifdef _WIN32
      std::remove(path.c_str());
#endif
    }
  };
  struct Run { uint64_t begin=0,end=0,rows=0; };
  class Writer {
   public:
    explicit Writer(ExternalSorter &owner):owner_(owner) { run_.begin=owner_.end_; }
    void add(const Row &row) {
      std::string packet=owner_.encode_(row);
      if (packet.empty() || packet.size()>owner_.maximum_record_)
        throw_error("RESOURCE_LIMIT","query sort record is too large");
      uint32_t length=static_cast<uint32_t>(packet.size());
      uint32_t checksum=crc32(packet.data(),packet.size());
      write(&length,sizeof(length));write(&checksum,sizeof(checksum));write(packet.data(),packet.size());
      ++run_.rows;
    }
    Run finish() { flush();owner_.sync_written();run_.end=owner_.end_;return run_; }
   private:
    void write(const void *data,size_t length) {
      const auto *bytes=static_cast<const uint8_t *>(data);
      while (length) {
        size_t count=std::min(length,buffer_.size()-used_);
        std::memcpy(buffer_.data()+used_,bytes,count);
        used_+=count;bytes+=count;length-=count;
        if (used_==buffer_.size()) flush();
      }
    }
    void flush() {
      if (!used_) return;
      if (used_>std::numeric_limits<uint64_t>::max()-owner_.end_ ||
          !owner_.scratch_.file.pwrite_full(buffer_.data(),used_,owner_.end_))
        throw_error("IO_ERROR","cannot write query sort run");
      owner_.end_+=used_;used_=0;
      if (owner_.end_-owner_.synced_>=8*1024*1024) owner_.sync_written();
    }
    ExternalSorter &owner_;
    Run run_;
    std::array<uint8_t,kBufferBytes> buffer_{};
    size_t used_=0;
  };
  class Cursor {
   public:
    Row row;
    explicit Cursor(Run run):run_(run),position_(run.begin) {}
    bool next(ExternalSorter &owner) {
      if (read_rows_==run_.rows) {
        if (position_!=run_.end) throw_error("CORRUPTION","trailing query sort data");
        return false;
      }
      uint32_t length=0,checksum=0;
      read(owner,&length,sizeof(length));read(owner,&checksum,sizeof(checksum));
      if (!length || length>owner.maximum_record_ || position_>run_.end || length>run_.end-position_)
        throw_error("CORRUPTION","invalid query sort record length");
      packet_.resize(length);read(owner,packet_.data(),packet_.size());
      if (crc32(packet_.data(),packet_.size())!=checksum)
        throw_error("CORRUPTION","query sort record checksum mismatch");
      row=owner.decode_(packet_);++read_rows_;return true;
    }
   private:
    void read(ExternalSorter &owner,void *data,size_t length) {
      if (position_>run_.end || length>run_.end-position_)
        throw_error("CORRUPTION","truncated query sort run");
      auto *bytes=static_cast<uint8_t *>(data);
      while (length) {
        if (within_==available_) {
          available_=static_cast<size_t>(std::min<uint64_t>(buffer_.size(),run_.end-position_));within_=0;
          if (!available_ || !owner.scratch_.file.pread_full(buffer_.data(),available_,position_))
            throw_error("IO_ERROR","cannot read query sort run");
          owner.scratch_.file.discard_clean_cache(position_,available_);
        }
        size_t count=std::min(length,available_-within_);
        std::memcpy(bytes,buffer_.data()+within_,count);
        within_+=count;position_+=count;bytes+=count;length-=count;
      }
    }
    Run run_;
    uint64_t position_,read_rows_=0;
    std::array<uint8_t,kBufferBytes> buffer_{};
    size_t within_=0,available_=0;
    std::string packet_;
  };
  void sync_written() {
    if (end_==synced_) return;
    // Buffered scratch pages count against a Linux cgroup too. Bound dirty
    // writes before discarding their clean cache; this is not a durability
    // requirement and changes neither table/WAL bytes nor commit ordering.
    if (!scratch_.file.fsync()) throw_error("IO_ERROR","cannot flush query sort run");
    scratch_.file.discard_clean_cache(synced_,end_-synced_);
    synced_=end_;
  }
  Run write_buffer() {
    std::sort(buffer_.begin(),buffer_.end(),less_);
    Writer writer(*this);
    for (const auto &row:buffer_) writer.add(row);
    return writer.finish();
  }
  void flush() {
    if (buffer_.empty()) return;
    Run run=write_buffer();
    buffer_.clear();extra_=0;
    add_run(run,0);
  }
  void add_run(Run run,size_t level) {
    if (levels_.size()<=level) levels_.resize(level+1);
    levels_[level].push_back(run);
    if (levels_[level].size()==kFanIn) {
      auto group=std::move(levels_[level]);
      levels_[level].clear();
      Run combined=merge_run(group);
      add_run(combined,level+1);
    }
  }
  Run merge_run(const std::vector<Run> &runs) {
    Writer writer(*this);
    merge(runs,0,std::numeric_limits<uint64_t>::max(),[&](const Row &row){writer.add(row);});
    return writer.finish();
  }
  void merge(const std::vector<Run> &runs,uint64_t skip,uint64_t take,
             const std::function<void(const Row &)> &emit) {
    if (runs.empty() || !take) return;
    std::vector<Cursor> cursors;cursors.reserve(runs.size());
    for (Run run:runs) cursors.emplace_back(run);
    auto later=[&](size_t a,size_t b){return less_(cursors[b].row,cursors[a].row);};
    std::priority_queue<size_t,std::vector<size_t>,decltype(later)> heap(later);
    for (size_t i=0;i<cursors.size();++i) if (cursors[i].next(*this)) heap.push(i);
    uint64_t seen=0,emitted=0;
    while (!heap.empty()) {
      size_t index=heap.top();heap.pop();
      if (seen>=skip) { emit(cursors[index].row);if (++emitted==take) break; }
      ++seen;
      if (cursors[index].next(*this)) heap.push(index);
    }
  }
  Scratch scratch_;
  size_t budget_;
  uint32_t maximum_record_;
  Less less_;
  Encode encode_;
  Decode decode_;
  Memory memory_;
  uint64_t end_=0;
  uint64_t synced_=0;
  size_t extra_=0;
  std::vector<Row> buffer_;
  std::vector<std::vector<Run>> levels_;
};
} // namespace mvd
