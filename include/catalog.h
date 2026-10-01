#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast.h"  // ColumnDef, ColumnType
#include "btree.h"
#include "buffer_pool.h"
#include "row_codec.h"
#include "schema.h"
#include "value.h"

namespace reldb {

// Thrown for anything that makes a CREATE TABLE / table lookup / insert
// invalid: duplicate or unknown table name, a schema that doesn't fit
// this catalog's fixed-size limits, a non-INTEGER primary key (this
// project's B+tree keys are int64_t — see FindTableIndex's caller
// InsertRow), or a row whose encoded size wouldn't fit VALUE_SIZE.
struct CatalogError : std::runtime_error {
  explicit CatalogError(const std::string& msg) : std::runtime_error(msg) {}
};

// Persistent, multi-table catalog built on top of the SAME BufferPool/WAL
// stack the single-table BTree/Database already use — the whole point is
// that recovery doesn't need to change at all: it replays page images
// without caring whether a page is a catalog page or a B+tree node, so
// multi-table support is "just" a new page layout (catalog_page.h) plus
// one BTree per table, each wired to persist its root_page_id back into
// this catalog page instead of a private metadata page (see BTree's
// catalog-managed constructor in btree.h/btree.cpp).
//
// Layout: the catalog page is always page_id 0 of a catalog-managed
// database file (the very first page NewPage() hands out on a fresh
// file). Each table gets its own BTree, rooted at a leaf page allocated
// at CREATE TABLE time; that BTree is only actually constructed in
// memory the first time the table is touched (GetTable) — "lazy" refers
// to the in-memory BTree object, not the on-disk root page, which exists
// from CREATE TABLE onward.
//
// Not thread-safe, consistent with BTree/BufferPool.
class Catalog {
 public:
  explicit Catalog(BufferPool* buffer_pool);

  // Registers a new table and allocates its (initially empty) backing
  // BTree. Throws CatalogError if: the name already exists; there's no
  // free table slot (MAX_CATALOG_TABLES, see catalog_page.h); the schema
  // has zero or more than one primary-key column, or the primary key
  // isn't INTEGER (this project's B+tree keys are int64_t, so a TEXT
  // primary key has nowhere to live); more than MAX_CATALOG_COLUMNS
  // columns; a table or column name too long for its fixed-width slot;
  // or the schema's encoded row size wouldn't fit VALUE_SIZE (checked via
  // RowCodec::EncodedSize()).
  void CreateTable(const std::string& table_name,
                    const std::vector<ColumnDef>& columns);

  bool TableExists(const std::string& table_name) const;

  const TableSchema& GetSchema(const std::string& table_name) const;

  // Constructs (if this is the first call for this table) or returns the
  // already-constructed BTree backing this table's rows, keyed by the
  // table's primary key column. Throws CatalogError if the table doesn't
  // exist.
  BTree& GetTable(const std::string& table_name);

  // Encodes `values` (schema column order, including the primary key's
  // own value) via this table's RowCodec and inserts into its BTree,
  // keyed on the primary key value, then durably bumps the catalog's
  // row-count for this table. Throws CatalogError on a wrong value count
  // or a type mismatch against the schema.
  void InsertRow(const std::string& table_name,
                 const std::vector<Value>& values);

  // Every row currently stored for `table_name`, decoded back into
  // Value vectors in schema column order, in primary-key order (this
  // project has no secondary indexing — a scan is a BTree range scan
  // over the whole key space).
  std::vector<std::vector<Value>> ScanTable(const std::string& table_name);

  // Reads the durable row count the catalog page stores for this table —
  // an exact count in this implementation, but treated by the planner's
  // cost estimator (milestone 4 task 14) the way a real optimizer treats
  // approximate statistics.
  uint64_t EstimatedRowCount(const std::string& table_name) const;

  std::vector<std::string> TableNames() const;

 private:
  struct TableHandle {
    bool in_use = false;
    TableSchema schema;
    std::unique_ptr<RowCodec> codec;
    std::unique_ptr<BTree> tree;  // null until first GetTable()
  };

  // -1 if no table by this name exists.
  int FindTableIndex(const std::string& table_name) const;

  // Persists a table's current root_page_id into its catalog slot. Passed
  // as the on_root_changed callback to every catalog-managed BTree this
  // class constructs.
  void WriteRootPageId(int slot_index, page_id_t new_root);

  void BumpRowCount(int slot_index);

  BufferPool* buffer_pool_;
  page_id_t catalog_page_id_ = 0;

  // Fixed at MAX_CATALOG_TABLES entries for the lifetime of this Catalog
  // (resized exactly once, in the constructor, and never again) so that
  // every TableHandle::schema's address stays stable — RowCodec holds a
  // reference to its TableSchema, not a copy, and that reference must
  // outlive the codec.
  std::vector<TableHandle> tables_;
};

}  // namespace reldb
