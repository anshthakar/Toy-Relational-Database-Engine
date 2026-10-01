#include <cstdio>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "ast.h"
#include "buffer_pool.h"
#include "catalog.h"
#include "disk_manager.h"
#include "value.h"

using namespace reldb;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      throw std::runtime_error(std::string("CHECK failed: ") + #cond +   \
                                " at " + __FILE__ + ":" +                \
                                std::to_string(__LINE__));                \
    }                                                                    \
  } while (0)

#define CHECK_THROWS(expr)                                               \
  do {                                                                   \
    bool threw = false;                                                  \
    try {                                                                \
      expr;                                                              \
    } catch (const std::exception&) {                                    \
      threw = true;                                                      \
    }                                                                    \
    CHECK(threw);                                                        \
  } while (0)

namespace {

std::string TempDbPath(const std::string& name) {
  return "/tmp/reldb_catalog_test_" + name + ".db";
}

void RemoveIfExists(const std::string& path) { unlink(path.c_str()); }

std::vector<ColumnDef> UsersSchema() {
  return {
      ColumnDef{"id", ColumnType::kInteger, /*is_primary_key=*/true},
      ColumnDef{"name", ColumnType::kText, false},
      ColumnDef{"age", ColumnType::kInteger, false},
  };
}

void TestCreateAndInsertAndScan() {
  std::string path = TempDbPath("basic");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  Catalog cat(&bp);

  CHECK(!cat.TableExists("users"));
  cat.CreateTable("users", UsersSchema());
  CHECK(cat.TableExists("users"));
  CHECK(cat.GetSchema("users").columns.size() == 3);

  cat.InsertRow("users",
                {Value::Int(1), Value::Text("alice"), Value::Int(30)});
  cat.InsertRow("users", {Value::Int(2), Value::Text("bob"), Value::Int(25)});
  cat.InsertRow("users",
                {Value::Int(3), Value::Text("carol"), Value::Int(40)});

  CHECK(cat.EstimatedRowCount("users") == 3);

  auto rows = cat.ScanTable("users");
  CHECK(rows.size() == 3);
  // Rows come back in primary-key order (BTree range scan order).
  CHECK(rows[0][0] == Value::Int(1));
  CHECK(rows[0][1] == Value::Text("alice"));
  CHECK(rows[0][2] == Value::Int(30));
  CHECK(rows[1][0] == Value::Int(2));
  CHECK(rows[2][0] == Value::Int(3));
  CHECK(rows[2][1] == Value::Text("carol"));
}

void TestCreateTableValidation() {
  std::string path = TempDbPath("validation");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  Catalog cat(&bp);

  cat.CreateTable("users", UsersSchema());

  // Duplicate name.
  CHECK_THROWS(cat.CreateTable("users", UsersSchema()));

  // No primary key.
  CHECK_THROWS(cat.CreateTable(
      "no_pk", {ColumnDef{"a", ColumnType::kInteger, false}}));

  // Two primary keys.
  CHECK_THROWS(cat.CreateTable(
      "two_pk", {ColumnDef{"a", ColumnType::kInteger, true},
                 ColumnDef{"b", ColumnType::kInteger, true}}));

  // TEXT primary key — this engine's BTree keys are int64_t only.
  CHECK_THROWS(cat.CreateTable(
      "text_pk", {ColumnDef{"a", ColumnType::kText, true}}));

  // Zero columns.
  CHECK_THROWS(cat.CreateTable("empty", {}));

  // Too many columns (MAX_CATALOG_COLUMNS == 8).
  {
    std::vector<ColumnDef> cols;
    cols.push_back(ColumnDef{"pk", ColumnType::kInteger, true});
    for (int i = 0; i < 8; ++i) {
      cols.push_back(ColumnDef{"c" + std::to_string(i), ColumnType::kInteger,
                                false});
    }
    CHECK_THROWS(cat.CreateTable("too_many_cols", cols));
  }

  // Row too wide to fit VALUE_SIZE: several long-ish TEXT columns.
  // TEXT_COLUMN_CAPACITY is 16 bytes/column, VALUE_SIZE is 120 — 8 TEXT
  // columns alone would be 128 bytes, already over budget.
  {
    std::vector<ColumnDef> cols;
    cols.push_back(ColumnDef{"pk", ColumnType::kInteger, true});
    for (int i = 0; i < 8; ++i) {
      cols.push_back(
          ColumnDef{"t" + std::to_string(i), ColumnType::kText, false});
    }
    CHECK_THROWS(cat.CreateTable("too_wide", cols));
  }

  // Operating on a table that was never created.
  CHECK_THROWS(cat.GetSchema("ghost"));
  CHECK_THROWS(cat.InsertRow("ghost", {Value::Int(1)}));
  CHECK_THROWS(cat.ScanTable("ghost"));
  CHECK_THROWS(cat.EstimatedRowCount("ghost"));
}

void TestInsertValidation() {
  std::string path = TempDbPath("insert_validation");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  Catalog cat(&bp);
  cat.CreateTable("users", UsersSchema());

  // Wrong number of values.
  CHECK_THROWS(cat.InsertRow("users", {Value::Int(1)}));

  // Primary key value isn't an integer.
  CHECK_THROWS(cat.InsertRow(
      "users", {Value::Text("x"), Value::Text("alice"), Value::Int(1)}));
}

void TestMultipleTablesIndependentBTreesAndSplits() {
  std::string path = TempDbPath("multi_table");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  Catalog cat(&bp);

  cat.CreateTable(
      "small", {ColumnDef{"id", ColumnType::kInteger, true},
                ColumnDef{"v", ColumnType::kInteger, false}});
  cat.CreateTable(
      "big", {ColumnDef{"id", ColumnType::kInteger, true},
              ColumnDef{"v", ColumnType::kInteger, false}});

  // "big" gets enough rows to force several leaf/internal splits in its
  // own BTree (LEAF_MAX_KEYS == 4 — see btree_node.h), while "small" stays
  // tiny. Each table's BTree is independent, and each root split must
  // persist back into THAT table's own catalog slot without disturbing
  // the other table's slot.
  for (int i = 0; i < 50; ++i) {
    cat.InsertRow("big", {Value::Int(i), Value::Int(i * 10)});
  }
  cat.InsertRow("small", {Value::Int(1), Value::Int(100)});

  CHECK(cat.EstimatedRowCount("big") == 50);
  CHECK(cat.EstimatedRowCount("small") == 1);

  auto big_rows = cat.ScanTable("big");
  CHECK(big_rows.size() == 50);
  for (int i = 0; i < 50; ++i) {
    CHECK(big_rows[i][0] == Value::Int(i));
    CHECK(big_rows[i][1] == Value::Int(i * 10));
  }

  auto small_rows = cat.ScanTable("small");
  CHECK(small_rows.size() == 1);
  CHECK(small_rows[0][1] == Value::Int(100));
}

// Simulates a clean reopen (no crash): destroy the BufferPool/Catalog,
// construct fresh ones over the same file, and confirm every table's
// schema, root pointer, row count, and data survived — proof that the
// catalog page's own writes (schema at CREATE TABLE, root_page_id on
// every split, row_count on every insert) are durable, not just
// in-memory bookkeeping that happens to work within one process run.
void TestReopenPersistsEverything() {
  std::string path = TempDbPath("reopen");
  RemoveIfExists(path);
  {
    DiskManager dm(path);
    BufferPool bp(64, &dm);
    Catalog cat(&bp);
    cat.CreateTable("users", UsersSchema());
    for (int i = 0; i < 30; ++i) {
      cat.InsertRow(
          "users",
          {Value::Int(i), Value::Text("u" + std::to_string(i)), Value::Int(20 + i)});
    }
    bp.FlushAll();
  }
  {
    DiskManager dm(path);
    BufferPool bp(64, &dm);
    Catalog cat(&bp);

    CHECK(cat.TableExists("users"));
    CHECK(cat.GetSchema("users").columns.size() == 3);
    CHECK(cat.EstimatedRowCount("users") == 30);

    auto rows = cat.ScanTable("users");
    CHECK(rows.size() == 30);
    CHECK(rows[0][0] == Value::Int(0));
    CHECK(rows[29][0] == Value::Int(29));
    CHECK(rows[15][1] == Value::Text("u15"));

    // The reopened catalog's BTree must still be usable for further
    // writes, not just reads — proves root_page_id was read back
    // correctly, not just the row count.
    cat.InsertRow("users", {Value::Int(30), Value::Text("u30"), Value::Int(50)});
    CHECK(cat.EstimatedRowCount("users") == 31);
  }
}

void TestTableNames() {
  std::string path = TempDbPath("table_names");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  Catalog cat(&bp);

  CHECK(cat.TableNames().empty());
  cat.CreateTable("a", {ColumnDef{"id", ColumnType::kInteger, true}});
  cat.CreateTable("b", {ColumnDef{"id", ColumnType::kInteger, true}});
  auto names = cat.TableNames();
  CHECK(names.size() == 2);
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    void (*fn)();
  };
  NamedTest tests[] = {
      {"TestCreateAndInsertAndScan", TestCreateAndInsertAndScan},
      {"TestCreateTableValidation", TestCreateTableValidation},
      {"TestInsertValidation", TestInsertValidation},
      {"TestMultipleTablesIndependentBTreesAndSplits",
       TestMultipleTablesIndependentBTreesAndSplits},
      {"TestReopenPersistsEverything", TestReopenPersistsEverything},
      {"TestTableNames", TestTableNames},
  };

  int failures = 0;
  for (const auto& t : tests) {
    try {
      t.fn();
      std::printf("[PASS] %s\n", t.name);
    } catch (const std::exception& e) {
      std::printf("[FAIL] %s: %s\n", t.name, e.what());
      failures++;
    }
  }
  return failures == 0 ? 0 : 1;
}
