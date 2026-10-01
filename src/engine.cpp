#include "engine.h"

#include <stdexcept>

#include "exec_row.h"
#include "expr_eval.h"
#include "logical_plan.h"
#include "parser.h"
#include "physical_plan.h"
#include "recovery.h"
#include "semantic.h"

namespace reldb {

Engine::Engine(const std::string& data_path, const std::string& wal_path,
               size_t pool_size)
    : disk_manager_(data_path), wal_manager_(wal_path) {
  RunRecovery(&disk_manager_, &wal_manager_);
  buffer_pool_ =
      std::make_unique<BufferPool>(pool_size, &disk_manager_, &wal_manager_);
  catalog_ = std::make_unique<Catalog>(buffer_pool_.get());
}

namespace {

void ExecuteCreateTable(const CreateTableStmt& stmt, Catalog* catalog) {
  catalog->CreateTable(stmt.table_name, stmt.columns);
}

void ExecuteInsert(const InsertStmt& stmt, Catalog* catalog) {
  const TableSchema& schema = catalog->GetSchema(stmt.table_name);
  std::vector<Value> values(schema.columns.size());

  if (stmt.columns.empty()) {
    // ValidateStatement already confirmed stmt.values.size() ==
    // schema.columns.size() — values are given in schema column order.
    for (size_t i = 0; i < schema.columns.size(); ++i) {
      values[i] = LiteralToValue(stmt.values[i]);
    }
  } else {
    // ValidateStatement already confirmed stmt.columns names every
    // schema column exactly once, so every ColumnIndex() lookup below
    // succeeds and every slot in `values` ends up filled.
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
      int idx = schema.ColumnIndex(stmt.columns[i]);
      values[static_cast<size_t>(idx)] = LiteralToValue(stmt.values[i]);
    }
  }

  catalog->InsertRow(stmt.table_name, values);
}

QueryResult ExecuteSelect(const SelectStmt& stmt, Catalog* catalog) {
  std::unique_ptr<LogicalNode> logical = BuildLogicalPlan(stmt);
  std::unique_ptr<PhysicalPlan> physical = BuildPhysicalPlan(*logical, catalog);

  QueryResult result;
  physical->Open();
  bool first_row = true;
  std::optional<ExecRow> row;
  while ((row = physical->Next()).has_value()) {
    if (first_row) {
      result.column_names.reserve(row->fields.size());
      for (const ExecRow::Field& f : row->fields) {
        result.column_names.push_back(f.column);
      }
      first_row = false;
    }
    std::vector<Value> values;
    values.reserve(row->fields.size());
    for (const ExecRow::Field& f : row->fields) {
      values.push_back(f.value);
    }
    result.rows.push_back(std::move(values));
  }
  physical->Close();
  return result;
}

}  // namespace

QueryResult Engine::ExecuteSQL(const std::string& sql) {
  std::unique_ptr<Statement> stmt = ParseSQL(sql);
  ValidateStatement(*stmt, *catalog_);

  switch (stmt->kind) {
    case StatementKind::kCreateTable:
      ExecuteCreateTable(static_cast<const CreateTableStmt&>(*stmt),
                          catalog_.get());
      return QueryResult{};
    case StatementKind::kInsert:
      ExecuteInsert(static_cast<const InsertStmt&>(*stmt), catalog_.get());
      return QueryResult{};
    case StatementKind::kSelect:
      return ExecuteSelect(static_cast<const SelectStmt&>(*stmt),
                            catalog_.get());
  }
  throw std::logic_error(
      "Engine::ExecuteSQL: unreachable — unknown StatementKind");
}

}  // namespace reldb
