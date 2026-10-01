#pragma once

#include <memory>
#include <string>
#include <vector>

#include "buffer_pool.h"
#include "catalog.h"
#include "disk_manager.h"
#include "value.h"
#include "wal.h"

namespace reldb {

// One statement's result. CREATE TABLE and INSERT produce no rows
// (both vectors stay empty — success is "didn't throw"). SELECT fills
// `column_names` with the output field names PhysicalProject assigned
// (see physical_plan.cpp) and `rows` with one vector<Value> per result
// row, in column_names order.
//
// column_names is derived from the first result row, so a SELECT with
// zero matching rows comes back with column_names empty too — a
// documented limitation (see docs/milestone-4.md): naming the columns
// without inspecting an actual row would mean a second, separate code
// path for deriving names from the SelectStmt/physical plan, which this
// project opts not to maintain just for the empty-result case.
struct QueryResult {
  std::vector<std::string> column_names;
  std::vector<std::vector<Value>> rows;
};

// Ties the parser (milestone 3) + semantic validation + logical/physical
// planner + Catalog/storage stack (milestone 4) together behind one
// entry point — ExecuteSQL(string) in, QueryResult out — the same way
// Database ties the single-table storage stack together for milestones
// 1/2. Built in PARALLEL to Database, not replacing it: Database/BTree's
// single-table behavior and every milestone 1/2 test that depends on it
// are untouched by this class's existence.
//
// Recovery sequencing is identical to Database's, for the identical
// reason: WALManager's constructor truncates any torn tail from a prior
// crash, then RunRecovery replays every intact record onto the data
// file, and only THEN does anything (here, Catalog, same as BTree in
// Database) start reading pages through the buffer pool.
class Engine {
 public:
  Engine(const std::string& data_path, const std::string& wal_path,
         size_t pool_size);

  // Parses, semantically validates, plans, and executes one SQL
  // statement end to end. Throws LexError/ParseError for bad syntax,
  // SemanticError for valid syntax that doesn't resolve against the
  // current catalog, or CatalogError for a CREATE TABLE that violates
  // this engine's schema constraints — never silently ignores a problem
  // or returns a partial result.
  QueryResult ExecuteSQL(const std::string& sql);

  Catalog& catalog() { return *catalog_; }

 private:
  DiskManager disk_manager_;
  WALManager wal_manager_;
  // Constructed in the constructor body, after recovery runs — same
  // ordering reasoning as Database's buffer_pool_/btree_ (see database.h).
  std::unique_ptr<BufferPool> buffer_pool_;
  std::unique_ptr<Catalog> catalog_;
};

}  // namespace reldb
