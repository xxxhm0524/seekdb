#include "workspace.h"
#include "../storage/store.h"
namespace mvd {
TransactionWorkspace::TransactionWorkspace(std::string path,size_t budget,Materialize materialize)
 :spool_path_(std::move(path)),budget_(budget),materialize_(std::move(materialize)){}
void TransactionWorkspace::begin(){
  if(active_)throw_error("TRANSACTION_ERROR","transaction already active");
  clear();active_=true;
}
void TransactionWorkspace::clear(){
  active_=false;tables_.clear();spool_.reset();resident_payload_=0;
  std::vector<uint8_t>().swap(encode_buffer_);
}
const TxTable* TransactionWorkspace::find_table(uint32_t id)const {
  auto it=tables_.find(id);return it==tables_.end()?nullptr:&it->second;
}
const RowState* TransactionWorkspace::find_row(uint32_t id,int64_t pk)const {
  auto table=find_table(id);if(!table)return nullptr;
  auto it=table->rows.find(pk);return it==table->rows.end()?nullptr:&it->second;
}
TxTable& TransactionWorkspace::table_ref(Table& table){
  auto old=tables_.find(table.id());
  if(undo_&&!undo_->tables.count(table.id()))
    undo_->tables.emplace(table.id(),UndoTable{old!=tables_.end(),old==tables_.end()?0:old->second.next_pos});
  auto [entry,inserted]=tables_.try_emplace(table.id());
  if(inserted)entry->second.next_pos=table.next_pos();
  return entry->second;
}
uint64_t TransactionWorkspace::allocate_position(Table& table){return table_ref(table).next_pos++;}
std::vector<uint8_t> TransactionWorkspace::row_buffer(){
  std::vector<uint8_t> row;if(spool_)row.swap(encode_buffer_);return row;
}
std::shared_ptr<const RowPatch> TransactionWorkspace::track_patch(RowPatch patch){
  size_t bytes=sizeof(RowPatch)+patch.capacity()*sizeof(RowPatch::value_type);
  for(const auto& change:patch)bytes+=change.second.s.capacity()+change.second.vec.capacity()*sizeof(float);
  auto owned=std::make_unique<RowPatch>(std::move(patch));auto budget=patch_budget_;budget->bytes+=bytes;
  return std::shared_ptr<const RowPatch>(owned.release(),[budget,bytes](const RowPatch* value){delete value;budget->bytes-=bytes;});
}
void TransactionWorkspace::prepare(StatementUndo& undo){
  undo.had_spool=bool(spool_);if(spool_)undo.spool_end=spool_->size();
}
void TransactionWorkspace::spill_row(RowState& state){
  if(state.row.empty())return;
  uint32_t length=static_cast<uint32_t>(state.row.size());auto offset=spool_->append(state.row);
  state.spool=spool_;state.spool_offset=offset;state.spool_size=length;
  encode_buffer_.swap(state.row);std::vector<uint8_t>().swap(state.row);
}
void TransactionWorkspace::spill_state(uint32_t table,RowState& state){
  if(state.deleted)return;
  if(!state.patches.empty()){
    std::vector<uint8_t> materialized;materialized.swap(encode_buffer_);
    materialize_(table,state,materialized);state.row=std::move(materialized);
    state.spool.reset();state.spool_size=0;state.patches.clear();
  }
  spill_row(state);
}
void TransactionWorkspace::stage(Table& table,int64_t pk,RowState state){
  auto& changes=table_ref(table);auto old=changes.rows.find(pk);
  if(undo_)undo_->rows.push_back(UndoRow{table.id(),pk,old!=changes.rows.end(),old==changes.rows.end()?RowState{}:old->second});
  size_t previous=old==changes.rows.end()?0:old->second.row.size();
  if(!spool_&&resident_payload_-previous+state.row.size()+patch_budget_->bytes>budget_){
    spool_=std::make_shared<TxSpool>(spool_path_);
    for(auto& entry:tables_)for(auto& row:entry.second.rows){size_t bytes=row.second.row.size();spill_state(entry.first,row.second);resident_payload_-=bytes;}
    previous=0;
  }
  if(spool_)spill_state(table.id(),state);
  resident_payload_=resident_payload_-previous+state.row.size();changes.rows[pk]=std::move(state);
}
void TransactionWorkspace::rollback(StatementUndo& undo){
  for(auto it=undo.rows.rbegin();it!=undo.rows.rend();++it){auto& rows=tables_.at(it->table).rows;if(it->existed)rows[it->pk]=std::move(it->before);else rows.erase(it->pk);}
  for(const auto& entry:undo.tables){if(entry.second.existed)tables_.at(entry.first).next_pos=entry.second.next_pos;else tables_.erase(entry.first);}
  resident_payload_=0;
  for(const auto& entry:tables_)for(const auto& row:entry.second.rows)resident_payload_+=row.second.row.size();
  if(spool_&&!undo.had_spool){
    uint64_t retained=0;for(const auto& entry:tables_)for(const auto& row:entry.second.rows)
      if(row.second.spool_size)retained=std::max(retained,row.second.spool_offset+row.second.spool_size);
    if(retained)spool_->truncate(retained);else spool_.reset();
  }else if(spool_)spool_->truncate(undo.spool_end);
}
}
