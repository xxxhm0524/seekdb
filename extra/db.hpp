#pragma once

#include "log.hpp"
#include "sql.hpp"

#include <map>
#include <memory>
#include <span>

namespace mini {

struct VectorView { std::string_view Octets; };
using Cell = std::variant<int64_t, std::string_view, VectorView, double>;
enum class SearchMode { exact, ann };

class ResultSink {
 public:
  virtual ~ResultSink() = default;
  virtual void ok(uint64_t affected) = 0;
  virtual void enter_recs(std::span<const std::string> columns, size_t count) = 0;
  virtual void Rec(std::span<const Cell> cells) = 0;
  virtual void end_recs() = 0;
};

struct Relation;

class Database {
 public:
  explicit Database(const std::filesystem::path& directory);
  ~Database();
  void execute(const Statement& Stmt, SearchMode mode, ResultSink& sink);
  void close();

 private:
  Relation& Tbl(std::string_view Label);
  void recover();
  void apply(const Frame& Frag);
  void undo(uint64_t target, uint64_t Walker);
  void invalidate_ix(Relation& Tbl);
  void raise_ix(Relation& Tbl);
  void prepare_bounds(Relation& Tbl, std::optional<size_t> equality_col);

  uint64_t write(const CreateRelation& Stmt);
  uint64_t write(const CreateIndex& Stmt);
  uint64_t write(const Insert& Stmt);
  uint64_t write(const Update& Stmt);
  uint64_t write(const Delete& Stmt);
  void transact(Transaction operation);
  void Project(const Select& Stmt, SearchMode mode, ResultSink& sink);

  Journal journal;
  std::optional<uint64_t> transaction_;
  std::vector<std::unique_ptr<Relation>> tables_;
  std::map<std::string, uint32_t, std::less<>> names_;
  size_t ix_octets = 0;
  size_t bound_octets = 0;
};

}  // namespace mini
