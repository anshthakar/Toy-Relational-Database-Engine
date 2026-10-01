#pragma once

#include <cstdint>
#include <cstring>

#include "page.h"

namespace reldb {

// On-disk layout of the single catalog page (always page_id 0 in a
// catalog-managed database — see Catalog in catalog.h). Same design
// philosophy as btree_node.h: a plain fixed-stride struct, no pointers,
// explicit reserved padding so every field — in particular the 8-byte
// fields — lands at an 8-byte-aligned offset. reinterpret_cast'ing raw
// page bytes into a struct with a misaligned 8-byte member is undefined
// behavior (UBSan catches it even on x86, where it happens to "work"),
// so the padding here is load-bearing, not cosmetic.
//
// Documented simplification: a fixed maximum number of tables and columns
// per table, each with a fixed-width name — this is a toy catalog, not a
// production system catalog with var-length entries. CREATE TABLE past
// MAX_CATALOG_TABLES, or with more than MAX_CATALOG_COLUMNS columns, or
// with over-length names, is rejected with a clear error (Catalog's job),
// not silently truncated or corrupted.
constexpr size_t MAX_CATALOG_TABLES = 8;
constexpr size_t MAX_CATALOG_COLUMNS = 8;
constexpr size_t CATALOG_TABLE_NAME_LEN = 32;
constexpr size_t CATALOG_COLUMN_NAME_LEN = 24;

#pragma pack(push, 1)

// One column declaration as recorded in the catalog. `type` stores
// ast::ColumnType as a raw uint8_t (not the enum itself) so this struct
// stays a plain, memcpy-safe POD independent of ast.h's enum layout —
// CatalogColumnEntry <-> ColumnDef conversion is explicit, in catalog.cpp.
struct CatalogColumnEntry {
  char name[CATALOG_COLUMN_NAME_LEN];
  uint8_t type;            // ColumnType::kInteger == 0, kText == 1
  uint8_t is_primary_key;  // 0 or 1
  uint8_t reserved[6];     // pads 24+1+1=26 up to 32 (multiple of 8)
};
static_assert(sizeof(CatalogColumnEntry) == 32,
              "CatalogColumnEntry must stay 8-byte-aligned-stride");

// One table's full entry: its schema plus where its data actually lives
// (root_page_id of its own, independent BTree) and a durable row-count
// estimate the cost estimator reads (see planner — milestone 4 task 14).
struct CatalogTableEntry {
  char table_name[CATALOG_TABLE_NAME_LEN];
  uint8_t num_columns;
  uint8_t in_use;  // 0 = empty slot (available for a new CREATE TABLE),
                   // 1 = active table. Slots are never compacted/reused
                   // across a Delete-table feature this project doesn't
                   // implement — simplification, not an oversight.
  uint8_t reserved1[6];  // pads 32+1+1=34 up to 40
  CatalogColumnEntry columns[MAX_CATALOG_COLUMNS];
  page_id_t root_page_id;
  uint8_t reserved2[4];  // pads root_page_id (4 bytes) up to 8
  uint64_t row_count;    // exact count, bumped on every successful insert
                          // through Catalog::InsertRow — not a sampled
                          // estimate, but still called an "estimate" where
                          // the cost model consumes it, since a real
                          // engine's statistics would be approximate.
};
static_assert(sizeof(CatalogTableEntry) ==
                  40 + MAX_CATALOG_COLUMNS * sizeof(CatalogColumnEntry) + 16,
              "CatalogTableEntry layout drifted from the hand-computed size");
static_assert(sizeof(CatalogTableEntry) % 8 == 0,
              "entry stride must be a multiple of 8 so every array "
              "element's internal fields stay aligned, not just the first");

// The full catalog page body, placed immediately after PageHeader.
struct CatalogBody {
  uint32_t num_tables;  // informational only (count of in_use == 1
                         // entries) — CreateTable/GetTable still scan all
                         // MAX_CATALOG_TABLES slots rather than trust it,
                         // since the authoritative state is `in_use`.
  uint8_t reserved[4];   // pads 4 up to 8
  CatalogTableEntry tables[MAX_CATALOG_TABLES];
};
static_assert(sizeof(CatalogBody) % 8 == 0,
              "catalog body size must stay a multiple of 8");

#pragma pack(pop)

constexpr size_t CATALOG_BODY_OFFSET = sizeof(PageHeader);
static_assert(CATALOG_BODY_OFFSET % 8 == 0,
              "catalog body must start 8-byte aligned");
static_assert(CATALOG_BODY_OFFSET + sizeof(CatalogBody) <= PAGE_SIZE,
              "catalog body must fit in a single 4096-byte page — lower "
              "MAX_CATALOG_TABLES/MAX_CATALOG_COLUMNS if this ever fires");

inline CatalogBody* GetCatalogBody(Page* page) {
  return reinterpret_cast<CatalogBody*>(page->data() + CATALOG_BODY_OFFSET);
}

}  // namespace reldb
