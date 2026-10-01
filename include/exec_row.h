#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "value.h"

namespace reldb {

// Thrown when a WHERE/ON/SELECT column reference doesn't resolve to
// exactly one field: unknown table/column qualifier, an unqualified name
// that doesn't exist in this row, or (post-JOIN) an unqualified name that
// exists in more than one joined table and so is genuinely ambiguous —
// this project doesn't silently pick the first match for that case, the
// same way a real SQL engine wouldn't.
struct ColumnResolutionError : std::runtime_error {
  explicit ColumnResolutionError(const std::string& msg)
      : std::runtime_error(msg) {}
};

// One row flowing through the executor (see physical_plan.h). Not just a
// vector<Value>: WHERE/ON expressions reference columns by name, and
// after a JOIN those names can collide across tables ("orders.id" vs.
// "customers.id"), so every field carries along which table (or alias)
// it came from.
struct ExecRow {
  struct Field {
    // Table name or alias. Never empty for a row flowing THROUGH the
    // pipeline (scans always tag their fields), so Find() below can rely
    // on it. The one exception is a PhysicalProject's final OUTPUT row
    // (see physical_plan.cpp): once a value has been projected out under
    // its SELECT-list name, nothing resolves it by qualifier again, so an
    // unqualified select item's output field leaves this empty.
    std::string table;
    std::string column;
    Value value;
  };
  std::vector<Field> fields;

  // Resolves a column reference per SQL's usual qualifier rules:
  //   - `table` given: only a field from that exact table/alias matches.
  //   - `table` absent: exactly one field named `column`, across every
  //     table this row has fields from, must match.
  // Throws ColumnResolutionError on zero or (unqualified case) more than
  // one match, rather than returning a null/first-match guess.
  const Value& Find(const std::optional<std::string>& table,
                     const std::string& column) const;
};

}  // namespace reldb
