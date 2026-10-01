#pragma once

#include <string>
#include <vector>

#include "ast.h"  // ColumnDef, ColumnType — the parser's declaration shape

namespace reldb {

// A table's resolved shape, as recorded in the catalog after a CREATE
// TABLE executes. Reuses ast.h's ColumnDef rather than defining a
// parallel struct — a column declaration doesn't change meaning between
// "what the parser read" and "what's now on file", so there's no reason
// for two types.
struct TableSchema {
  std::string table_name;
  std::vector<ColumnDef> columns;  // includes the primary key column

  // Index into `columns` of the (exactly one, enforced at CREATE TABLE
  // time) primary key column, or -1 if this schema was never validated
  // (shouldn't happen for anything that made it into the catalog).
  int PrimaryKeyIndex() const {
    for (size_t i = 0; i < columns.size(); ++i) {
      if (columns[i].is_primary_key) return static_cast<int>(i);
    }
    return -1;
  }

  // -1 if no column with this name exists.
  int ColumnIndex(const std::string& name) const {
    for (size_t i = 0; i < columns.size(); ++i) {
      if (columns[i].name == name) return static_cast<int>(i);
    }
    return -1;
  }
};

}  // namespace reldb
