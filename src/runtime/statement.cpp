#include "../runtime/database.h"
#include "../runtime/shared.h"
#include "../sql.h"
#include <algorithm>
namespace mvd::detail {
Response Database::execute(const std::string &sql) {
  Response resp;
  std::unique_ptr<Statement> stmt;
  try {
    if(lifecycle_==Lifecycle::closed)throw_error("IO_ERROR","database is not open");
    if (lifecycle_==Lifecycle::recovery_required) recover_commit_publication();
    SqlParser parser(sql);
    stmt = parser.parse();
  } catch (const DbError &e) {
    resp.ok = false;
    resp.error_code = e.code;
    resp.error_message = e.message;
    return resp;
  } catch (const std::exception &e) {
    resp.ok = false;
    resp.error_code = "INTERNAL_ERROR";
    resp.error_message = e.what();
    return resp;
  }

  auto txn_guard = [&](auto fn) -> Response {
    return tx_.execute_statement(fn,[&]{commit_transaction();});
  };

  try {
    switch (stmt->kind) {
      case Statement::Kind::CREATE_TABLE:
        if (tx_.active())
          throw_error("UNSUPPORTED", "DDL inside a transaction is not supported");
        return exec_create_table(*static_cast<CreateTableStmt *>(stmt.get()));
      case Statement::Kind::DROP_TABLE:
        if (tx_.active())
          throw_error("UNSUPPORTED", "DDL inside a transaction is not supported");
        return exec_drop_table(*static_cast<DropTableStmt *>(stmt.get()));
      case Statement::Kind::CREATE_VECTOR_INDEX:
        if (tx_.active())
          throw_error("UNSUPPORTED", "DDL inside a transaction is not supported");
        return exec_create_vector_index(
            *static_cast<CreateVectorIndexStmt *>(stmt.get()));
      case Statement::Kind::INSERT:
        return txn_guard([&]() { return exec_insert(*static_cast<InsertStmt *>(stmt.get())); });
      case Statement::Kind::UPDATE:
        return txn_guard([&]() { return exec_update(*static_cast<UpdateStmt *>(stmt.get())); });
      case Statement::Kind::DELETE:
        return txn_guard([&]() { return exec_delete(*static_cast<DeleteStmt *>(stmt.get())); });
      case Statement::Kind::SELECT:
        return exec_select(*static_cast<SelectStmt *>(stmt.get()));
      case Statement::Kind::BEGIN:
        return exec_begin();
      case Statement::Kind::COMMIT:
        return exec_commit();
      case Statement::Kind::ROLLBACK:
        return exec_rollback();
      case Statement::Kind::SET:
        return exec_set(*static_cast<SetStmt *>(stmt.get()));
    }
  } catch (const DbError &e) {
    resp.ok = false;
    resp.error_code = e.code;
    resp.error_message = e.message;
    return resp;
  } catch (const std::exception &e) {
    resp.ok = false;
    resp.error_code = "INTERNAL_ERROR";
    resp.error_message = e.what();
    return resp;
  }
  resp.ok = false;
  resp.error_code = "INTERNAL_ERROR";
  resp.error_message = "unhandled statement";
  return resp;
}

Response Database::exec_set(SetStmt &stmt) {
  if (stmt.name == "vector_mode" || stmt.name == "search_mode") {
    if (stmt.value == "ann" || stmt.value == "approximate") ann_mode_ = true;
    else if (stmt.value == "exact") ann_mode_ = false;
    else throw_error("INVALID_VALUE", "search mode must be exact or ann");
    return Response{};
  }
  throw_error("UNSUPPORTED", "unsupported SET variable: " + stmt.name);
}

Response Database::exec_begin() {
  tx_.begin();return Response{};
}

Response Database::exec_commit() {
  if (!tx_.active()) return Response{};
  commit_transaction();
  return Response{};
}

Response Database::exec_rollback() {
  rollback_transaction();
  return Response{};
}


} // namespace mvd::detail
