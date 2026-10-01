#include "catalog.h"

#include <cstring>
#include <limits>

#include "btree_node.h"  // PageType::LEAF, GetNodeExtra, VALUE_SIZE
#include "catalog_page.h"

namespace reldb {

namespace {

ColumnDef ColumnDefFromEntry(const CatalogColumnEntry& entry) {
  ColumnDef def;
  def.name.assign(entry.name, strnlen(entry.name, CATALOG_COLUMN_NAME_LEN));
  def.type = entry.type == 0 ? ColumnType::kInteger : ColumnType::kText;
  def.is_primary_key = entry.is_primary_key != 0;
  return def;
}

void FillColumnEntry(const ColumnDef& col, CatalogColumnEntry* entry) {
  std::memset(entry, 0, sizeof(CatalogColumnEntry));
  std::memcpy(entry->name, col.name.data(), col.name.size());
  entry->type = col.type == ColumnType::kInteger ? 0 : 1;
  entry->is_primary_key = col.is_primary_key ? 1 : 0;
}

}  // namespace

Catalog::Catalog(BufferPool* buffer_pool) : buffer_pool_(buffer_pool) {
  tables_.resize(MAX_CATALOG_TABLES);

  if (buffer_pool_->DiskPageCount() == 0) {
    PageGuard guard = buffer_pool_->NewPage();
    if (!guard.valid()) {
      throw CatalogError("Catalog: buffer pool exhausted creating the "
                          "catalog page");
    }
    catalog_page_id_ = guard.page_id();
    if (catalog_page_id_ != 0) {
      // Every catalog-managed database must allocate the catalog page
      // first, before any table's BTree root — this is what makes
      // "catalog page == page_id 0" a safe assumption to hardcode
      // elsewhere, rather than something that has to be looked up.
      throw std::logic_error(
          "Catalog: must be the first thing constructed against a fresh "
          "database — expected catalog page at page_id 0, got " +
          std::to_string(catalog_page_id_));
    }
    guard.page()->header()->page_type = PageType::CATALOG;
    // CatalogBody is already all-zero (BufferPool::NewPage() hands back a
    // zero-initialized page) — num_tables == 0 and every slot's in_use ==
    // 0 already hold, nothing further to write.
    guard.MarkDirty();
    return;
  }

  catalog_page_id_ = 0;
  PageGuard guard = buffer_pool_->FetchPage(catalog_page_id_);
  if (!guard.valid()) {
    throw CatalogError("Catalog: could not read the catalog page");
  }
  CatalogBody* body = GetCatalogBody(guard.page());
  for (size_t i = 0; i < MAX_CATALOG_TABLES; ++i) {
    const CatalogTableEntry& entry = body->tables[i];
    if (!entry.in_use) continue;

    TableHandle& handle = tables_[i];
    handle.in_use = true;
    handle.schema.table_name.assign(
        entry.table_name, strnlen(entry.table_name, CATALOG_TABLE_NAME_LEN));
    for (uint8_t c = 0; c < entry.num_columns; ++c) {
      handle.schema.columns.push_back(ColumnDefFromEntry(entry.columns[c]));
    }
    // handle.tree stays null: constructed lazily on first GetTable().
    handle.codec = std::make_unique<RowCodec>(handle.schema);
  }
}

int Catalog::FindTableIndex(const std::string& table_name) const {
  for (size_t i = 0; i < tables_.size(); ++i) {
    if (tables_[i].in_use && tables_[i].schema.table_name == table_name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool Catalog::TableExists(const std::string& table_name) const {
  return FindTableIndex(table_name) >= 0;
}

const TableSchema& Catalog::GetSchema(const std::string& table_name) const {
  int idx = FindTableIndex(table_name);
  if (idx < 0) {
    throw CatalogError("Catalog: no such table '" + table_name + "'");
  }
  return tables_[idx].schema;
}

void Catalog::CreateTable(const std::string& table_name,
                           const std::vector<ColumnDef>& columns) {
  if (table_name.empty() || table_name.size() >= CATALOG_TABLE_NAME_LEN) {
    throw CatalogError("Catalog::CreateTable: table name '" + table_name +
                        "' is empty or too long (max " +
                        std::to_string(CATALOG_TABLE_NAME_LEN - 1) +
                        " bytes)");
  }
  if (TableExists(table_name)) {
    throw CatalogError("Catalog::CreateTable: table '" + table_name +
                        "' already exists");
  }
  if (columns.empty() || columns.size() > MAX_CATALOG_COLUMNS) {
    throw CatalogError("Catalog::CreateTable: table '" + table_name +
                        "' must have between 1 and " +
                        std::to_string(MAX_CATALOG_COLUMNS) + " columns");
  }

  int pk_count = 0;
  int pk_index = -1;
  for (size_t i = 0; i < columns.size(); ++i) {
    const ColumnDef& col = columns[i];
    if (col.name.empty() || col.name.size() >= CATALOG_COLUMN_NAME_LEN) {
      throw CatalogError("Catalog::CreateTable: column name '" + col.name +
                          "' is empty or too long (max " +
                          std::to_string(CATALOG_COLUMN_NAME_LEN - 1) +
                          " bytes)");
    }
    if (col.is_primary_key) {
      pk_count++;
      pk_index = static_cast<int>(i);
    }
  }
  if (pk_count != 1) {
    throw CatalogError("Catalog::CreateTable: table '" + table_name +
                        "' must have exactly one PRIMARY KEY column, found " +
                        std::to_string(pk_count));
  }
  // This project's BTree keys are int64_t (see btree.h) — a TEXT primary
  // key has no representation as a BTree key, so it's rejected here
  // rather than silently misbehaving later at the first INSERT.
  if (columns[pk_index].type != ColumnType::kInteger) {
    throw CatalogError(
        "Catalog::CreateTable: primary key column '" +
        columns[pk_index].name +
        "' must be INTEGER — this engine's storage layer keys rows by a "
        "64-bit integer, so a TEXT primary key can't be stored");
  }

  TableSchema schema;
  schema.table_name = table_name;
  schema.columns = columns;
  // Checked against a throwaway RowCodec here purely for the size check —
  // NOT the codec that ends up stored (see below for why that one has to
  // be built against the TableHandle's own, address-stable schema).
  {
    RowCodec size_check_codec(schema);
    if (size_check_codec.EncodedSize() > VALUE_SIZE) {
      throw CatalogError(
          "Catalog::CreateTable: table '" + table_name +
          "' row encodes to " +
          std::to_string(size_check_codec.EncodedSize()) +
          " bytes, exceeding this engine's fixed VALUE_SIZE (" +
          std::to_string(VALUE_SIZE) +
          ") — fewer or narrower non-primary-key columns are needed");
    }
  }

  int slot = -1;
  for (size_t i = 0; i < tables_.size(); ++i) {
    if (!tables_[i].in_use) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot < 0) {
    throw CatalogError("Catalog::CreateTable: catalog is full (max " +
                        std::to_string(MAX_CATALOG_TABLES) + " tables)");
  }

  // Allocate the table's initial (empty) root leaf page FIRST, in its own
  // scope, so it is fully logged before the catalog page is written to
  // reference it — the same cross-page write-ahead ordering argument as
  // BTree::Insert's root-split (see btree.cpp): a crash between the two
  // writes must never leave a catalog entry pointing at a root page that
  // was never itself durably written.
  page_id_t root_id;
  {
    PageGuard root_guard = buffer_pool_->NewPage();
    if (!root_guard.valid()) {
      throw CatalogError(
          "Catalog::CreateTable: buffer pool exhausted allocating table "
          "root page");
    }
    root_guard.page()->header()->page_type = PageType::LEAF;
    root_guard.page()->header()->num_slots = 0;
    GetNodeExtra(root_guard.page())->next_leaf = INVALID_PAGE_ID;
    root_guard.MarkDirty();
    root_id = root_guard.page_id();
  }  // root_guard destructs here: logged BEFORE the catalog references it

  {
    PageGuard cat_guard = buffer_pool_->FetchPage(catalog_page_id_);
    CatalogBody* body = GetCatalogBody(cat_guard.page());
    CatalogTableEntry* entry = &body->tables[slot];
    std::memset(entry, 0, sizeof(CatalogTableEntry));
    std::memcpy(entry->table_name, table_name.data(), table_name.size());
    entry->num_columns = static_cast<uint8_t>(columns.size());
    entry->in_use = 1;
    for (size_t i = 0; i < columns.size(); ++i) {
      FillColumnEntry(columns[i], &entry->columns[i]);
    }
    entry->root_page_id = root_id;
    entry->row_count = 0;
    body->num_tables += 1;
    cat_guard.MarkDirty();
  }  // cat_guard destructs here: logged AFTER, safe to reference root_id

  TableHandle& handle = tables_[slot];
  handle.in_use = true;
  handle.schema = std::move(schema);
  // Built against handle.schema (a TableHandle inside the fixed-size,
  // never-reallocated tables_ vector — see the class comment), NOT the
  // now-moved-from local `schema` above: RowCodec stores a reference, not
  // a copy, so it must point at something whose address outlives it.
  // Constructing it against the local first (now fixed) caused a stack
  // use-after-return that ASan caught immediately.
  handle.codec = std::make_unique<RowCodec>(handle.schema);
  handle.tree.reset();  // constructed lazily by GetTable()
}

BTree& Catalog::GetTable(const std::string& table_name) {
  int slot = FindTableIndex(table_name);
  if (slot < 0) {
    throw CatalogError("Catalog::GetTable: no such table '" + table_name +
                        "'");
  }
  TableHandle& handle = tables_[slot];
  if (!handle.tree) {
    PageGuard cat_guard = buffer_pool_->FetchPage(catalog_page_id_);
    CatalogBody* body = GetCatalogBody(cat_guard.page());
    page_id_t root_id = body->tables[slot].root_page_id;
    handle.tree = std::make_unique<BTree>(
        buffer_pool_, root_id,
        [this, slot](page_id_t new_root) { WriteRootPageId(slot, new_root); });
  }
  return *handle.tree;
}

void Catalog::WriteRootPageId(int slot_index, page_id_t new_root) {
  PageGuard guard = buffer_pool_->FetchPage(catalog_page_id_);
  CatalogBody* body = GetCatalogBody(guard.page());
  body->tables[slot_index].root_page_id = new_root;
  guard.MarkDirty();
}

void Catalog::BumpRowCount(int slot_index) {
  PageGuard guard = buffer_pool_->FetchPage(catalog_page_id_);
  CatalogBody* body = GetCatalogBody(guard.page());
  body->tables[slot_index].row_count += 1;
  guard.MarkDirty();
}

void Catalog::InsertRow(const std::string& table_name,
                         const std::vector<Value>& values) {
  int slot = FindTableIndex(table_name);
  if (slot < 0) {
    throw CatalogError("Catalog::InsertRow: no such table '" + table_name +
                        "'");
  }
  TableHandle& handle = tables_[slot];
  if (values.size() != handle.schema.columns.size()) {
    throw CatalogError(
        "Catalog::InsertRow: table '" + table_name + "' expects " +
        std::to_string(handle.schema.columns.size()) + " values, got " +
        std::to_string(values.size()));
  }
  int pk_index = handle.schema.PrimaryKeyIndex();
  const Value& pk_value = values[pk_index];
  if (pk_value.kind != ValueKind::kInteger) {
    throw CatalogError("Catalog::InsertRow: primary key value for table '" +
                        table_name + "' must be an integer");
  }
  int64_t key = pk_value.int_value;

  std::string blob = handle.codec->Encode(values);
  BTree& tree = GetTable(table_name);  // ensures handle.tree exists
  tree.Insert(key, blob);
  BumpRowCount(slot);
}

std::vector<std::vector<Value>> Catalog::ScanTable(
    const std::string& table_name) {
  int slot = FindTableIndex(table_name);
  if (slot < 0) {
    throw CatalogError("Catalog::ScanTable: no such table '" + table_name +
                        "'");
  }
  TableHandle& handle = tables_[slot];
  BTree& tree = GetTable(table_name);
  auto pairs = tree.RangeScan(std::numeric_limits<int64_t>::min(),
                               std::numeric_limits<int64_t>::max());

  std::vector<std::vector<Value>> rows;
  rows.reserve(pairs.size());
  for (const auto& [key, blob] : pairs) {
    rows.push_back(handle.codec->Decode(key, blob));
  }
  return rows;
}

uint64_t Catalog::EstimatedRowCount(const std::string& table_name) const {
  int slot = FindTableIndex(table_name);
  if (slot < 0) {
    throw CatalogError("Catalog::EstimatedRowCount: no such table '" +
                        table_name + "'");
  }
  PageGuard guard = buffer_pool_->FetchPage(catalog_page_id_);
  const CatalogBody* body = GetCatalogBody(guard.page());
  return body->tables[slot].row_count;
}

std::vector<std::string> Catalog::TableNames() const {
  std::vector<std::string> names;
  for (const auto& handle : tables_) {
    if (handle.in_use) names.push_back(handle.schema.table_name);
  }
  return names;
}

}  // namespace reldb
