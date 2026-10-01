#pragma once

#include <stdexcept>
#include <string>

#include "ast.h"
#include "catalog.h"

namespace reldb {

// Thrown for a statement that's syntactically valid (the parser accepted
// it) but semantically wrong against the current catalog: an unknown
// table, an unknown or ambiguous column reference, a duplicate table
// alias, or an INSERT whose column/value counts don't line up.
struct SemanticError : std::runtime_error {
  explicit SemanticError(const std::string& msg) : std::runtime_error(msg) {}
};

// Validates `stmt` against `catalog`'s current schemas BEFORE any
// logical/physical planning or execution happens, so a bad query fails
// with a clear message up front rather than from deep inside the planner
// or (worse) from BTree/RowCodec reacting to garbage input.
//
// CREATE TABLE's own shape constraints (exactly one INTEGER primary key,
// column count/width limits, row-size-fits-VALUE_SIZE) are intentionally
// NOT re-checked here — Catalog::CreateTable is already the single source
// of truth for them, and duplicating those checks here would just be two
// places that could drift out of sync.
void ValidateStatement(const Statement& stmt, const Catalog& catalog);

}  // namespace reldb
