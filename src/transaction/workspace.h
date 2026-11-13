#pragma once
#include "state.h"
#include "../common.h"
#include <functional>
#include <type_traits>

namespace mvd {
class Table;

// Owns every uncommitted byte and savepoint. Views remain valid only until
// the next workspace mutation; callers cannot mutate accounting or deltas.
class TransactionWorkspace {
 public:
  using Materialize=std::function<void(uint32_t,const RowState&,std::vector<uint8_t>&)>;
  TransactionWorkspace(std::string spool_path,size_t budget,Materialize materialize);
  TransactionWorkspace(const TransactionWorkspace&)=delete;
  TransactionWorkspace& operator=(const TransactionWorkspace&)=delete;
  bool active()const noexcept{return active_;}
  bool spilled()const noexcept{return bool(spool_);}
  size_t resident_bytes()const noexcept{return resident_payload_;}
  const auto& tables()const noexcept{return tables_;}
  const TxTable* find_table(uint32_t id)const;
  const RowState* find_row(uint32_t id,int64_t pk)const;
  void begin();
  void clear();
  uint64_t allocate_position(Table&);
  void stage(Table&,int64_t pk,RowState);
  std::vector<uint8_t> row_buffer();
  std::shared_ptr<const RowPatch> track_patch(RowPatch);

  template<class Action,class Commit>
  auto execute_statement(Action&& action,Commit&& commit) {
    using Result=std::invoke_result_t<Action&>;
    static_assert(std::is_nothrow_move_constructible_v<Result>);
    if(statement_running_)throw_error("INTERNAL_ERROR","nested statement workspace");
    const bool autocommit=!active_;
    if(autocommit)begin();
    StatementUndo undo;
    if(!autocommit)prepare(undo);
    statement_running_=true;
    undo_=autocommit?nullptr:&undo;
    try {
      Result result=action();
      undo_=nullptr;
      if(autocommit){commit();clear();}
      statement_running_=false;
      return result;
    }catch(...) {
      undo_=nullptr;
      statement_running_=false;
      if(autocommit)clear();
      else rollback(undo);
      throw;
    }
  }

 private:
  struct PatchBudget {size_t bytes=0;};
  struct UndoRow {uint32_t table;int64_t pk;bool existed;RowState before;};
  struct UndoTable {bool existed;uint64_t next_pos;};
  struct StatementUndo {
    std::vector<UndoRow> rows;
    std::unordered_map<uint32_t,UndoTable> tables;
    bool had_spool=false;
    uint64_t spool_end=0;
  };
  TxTable& table_ref(Table&);
  void prepare(StatementUndo&);
  void rollback(StatementUndo&);
  void spill_row(RowState&);
  void spill_state(uint32_t table,RowState&);
  bool active_=false,statement_running_=false;
  std::unordered_map<uint32_t,TxTable> tables_;
  std::shared_ptr<TxSpool> spool_;
  size_t resident_payload_=0;
  std::vector<uint8_t> encode_buffer_;
  std::shared_ptr<PatchBudget> patch_budget_=std::make_shared<PatchBudget>();
  StatementUndo* undo_=nullptr;
  std::string spool_path_;
  size_t budget_;
  Materialize materialize_;
};
}
