#pragma once
#include "result.hpp"
#include "sql.hpp"
#include "storage.hpp"
#include "vector_index.hpp"
namespace mvd {
struct Predicate { size_t column; std::string op; Value value; };
std::vector<Predicate> bind_predicates(const Table& table, const std::vector<Condition>& conditions);
bool matches(const Row& row, const std::vector<Predicate>& predicates);
class Engine {
    std::filesystem::path directory_;
    Storage storage_;
    Catalog active_;
    bool transaction_ = false;
    std::unique_ptr<VectorIndex> vector_index_;
    std::optional<uint64_t> refused_vector_root_;
    std::string refused_vector_table_;
    std::vector<bool> refused_vector_columns_;
    size_t table_index(const std::string& name) const;
    Result select(const Statement& statement);
public:
    explicit Engine(const std::filesystem::path& directory);
    Result execute(const std::string& sql, const std::string& mode);
    void shutdown();
};
}
