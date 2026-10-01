#include "semantic.h"

#include <set>
#include <unordered_map>

namespace reldb {

namespace {

using AliasMap = std::unordered_map<std::string, const TableSchema*>;

void ValidateColumnRefsIn(const Expr& expr, const AliasMap& aliases) {
  switch (expr.kind) {
    case ExprKind::kColumnRef: {
      const auto& ref = static_cast<const ColumnRefExpr&>(expr);
      if (ref.table.has_value()) {
        auto it = aliases.find(*ref.table);
        if (it == aliases.end()) {
          throw SemanticError("unknown table or alias '" + *ref.table + "'");
        }
        if (it->second->ColumnIndex(ref.column) < 0) {
          throw SemanticError("no column '" + ref.column + "' in table '" +
                               *ref.table + "'");
        }
      } else {
        int matches = 0;
        for (const auto& [alias, schema] : aliases) {
          (void)alias;
          if (schema->ColumnIndex(ref.column) >= 0) matches++;
        }
        if (matches == 0) {
          throw SemanticError("no column '" + ref.column +
                               "' in any table referenced by this query");
        }
        if (matches > 1) {
          throw SemanticError(
              "column '" + ref.column +
              "' is ambiguous — it exists in more than one joined table; "
              "qualify it (e.g. table." +
              ref.column + ")");
        }
      }
      return;
    }
    case ExprKind::kLiteral:
      return;
    case ExprKind::kComparison: {
      const auto& cmp = static_cast<const ComparisonExpr&>(expr);
      ValidateColumnRefsIn(*cmp.left, aliases);
      ValidateColumnRefsIn(*cmp.right, aliases);
      return;
    }
    case ExprKind::kAnd: {
      const auto& a = static_cast<const AndExpr&>(expr);
      ValidateColumnRefsIn(*a.left, aliases);
      ValidateColumnRefsIn(*a.right, aliases);
      return;
    }
    case ExprKind::kOr: {
      const auto& o = static_cast<const OrExpr&>(expr);
      ValidateColumnRefsIn(*o.left, aliases);
      ValidateColumnRefsIn(*o.right, aliases);
      return;
    }
  }
}

void ValidateSelect(const SelectStmt& stmt, const Catalog& catalog) {
  AliasMap aliases;

  auto add_table = [&](const TableRef& ref) {
    if (!catalog.TableExists(ref.table)) {
      throw SemanticError("no such table '" + ref.table + "'");
    }
    std::string alias = ref.alias.has_value() ? *ref.alias : ref.table;
    if (aliases.count(alias)) {
      throw SemanticError("duplicate table name or alias '" + alias +
                           "' in FROM/JOIN — every table reference needs a "
                           "unique name");
    }
    aliases[alias] = &catalog.GetSchema(ref.table);
  };

  add_table(stmt.from);
  for (const JoinClause& join : stmt.joins) {
    add_table(join.table);
    ValidateColumnRefsIn(*join.on_condition, aliases);
  }

  if (!stmt.is_star) {
    for (const SelectItem& item : stmt.select_items) {
      if (item.table.has_value()) {
        auto it = aliases.find(*item.table);
        if (it == aliases.end()) {
          throw SemanticError("unknown table or alias '" + *item.table +
                               "' in SELECT list");
        }
        if (it->second->ColumnIndex(item.column) < 0) {
          throw SemanticError("no column '" + item.column + "' in table '" +
                               *item.table + "'");
        }
      } else {
        int matches = 0;
        for (const auto& [alias, schema] : aliases) {
          (void)alias;
          if (schema->ColumnIndex(item.column) >= 0) matches++;
        }
        if (matches == 0) {
          throw SemanticError("no column '" + item.column +
                               "' in any table referenced by this query");
        }
        if (matches > 1) {
          throw SemanticError("column '" + item.column +
                               "' in SELECT list is ambiguous — qualify it");
        }
      }
    }
  }

  if (stmt.where != nullptr) {
    ValidateColumnRefsIn(*stmt.where, aliases);
  }
}

void ValidateInsert(const InsertStmt& stmt, const Catalog& catalog) {
  if (!catalog.TableExists(stmt.table_name)) {
    throw SemanticError("no such table '" + stmt.table_name + "'");
  }
  const TableSchema& schema = catalog.GetSchema(stmt.table_name);

  if (stmt.columns.empty()) {
    if (stmt.values.size() != schema.columns.size()) {
      throw SemanticError(
          "INSERT into '" + stmt.table_name + "' expects " +
          std::to_string(schema.columns.size()) + " values (in schema "
          "column order), got " + std::to_string(stmt.values.size()));
    }
    return;
  }

  // Explicit column list: this engine requires it to name every column
  // exactly once (a documented simplification — no partial inserts with
  // defaults for omitted columns, since this storage layer has no
  // concept of a column default).
  if (stmt.columns.size() != schema.columns.size()) {
    throw SemanticError(
        "INSERT into '" + stmt.table_name + "' must name all " +
        std::to_string(schema.columns.size()) +
        " columns — partial inserts aren't supported");
  }
  if (stmt.columns.size() != stmt.values.size()) {
    throw SemanticError(
        "INSERT into '" + stmt.table_name +
        "': column list and VALUES list have different lengths");
  }
  std::set<std::string> seen;
  for (const std::string& name : stmt.columns) {
    if (schema.ColumnIndex(name) < 0) {
      throw SemanticError("no column '" + name + "' in table '" +
                           stmt.table_name + "'");
    }
    if (!seen.insert(name).second) {
      throw SemanticError("column '" + name +
                           "' is specified more than once in this INSERT's "
                           "column list");
    }
  }
}

}  // namespace

void ValidateStatement(const Statement& stmt, const Catalog& catalog) {
  switch (stmt.kind) {
    case StatementKind::kCreateTable:
      return;  // Catalog::CreateTable enforces its own constraints.
    case StatementKind::kInsert:
      ValidateInsert(static_cast<const InsertStmt&>(stmt), catalog);
      return;
    case StatementKind::kSelect:
      ValidateSelect(static_cast<const SelectStmt&>(stmt), catalog);
      return;
  }
}

}  // namespace reldb
