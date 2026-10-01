#include <cstdio>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "engine.h"
#include "parser.h"
#include "semantic.h"
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
  return "/tmp/reldb_executor_test_" + name + ".db";
}
std::string TempWalPath(const std::string& name) {
  return "/tmp/reldb_executor_test_" + name + ".wal";
}

void RemoveIfExists(const std::string& path) { unlink(path.c_str()); }

void TestEndToEndCreateInsertSelect() {
  std::string db = TempDbPath("basic");
  std::string wal = TempWalPath("basic");
  RemoveIfExists(db);
  RemoveIfExists(wal);

  Engine engine(db, wal, 64);

  engine.ExecuteSQL(
      "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);");
  engine.ExecuteSQL("INSERT INTO users VALUES (1, 'alice', 30);");
  engine.ExecuteSQL("INSERT INTO users VALUES (2, 'bob', 25);");
  engine.ExecuteSQL(
      "INSERT INTO users (id, age, name) VALUES (3, 40, 'carol');");

  QueryResult result = engine.ExecuteSQL("SELECT * FROM users WHERE age = 30;");
  CHECK(result.rows.size() == 1);
  CHECK(result.column_names.size() == 3);
  // Find the "name" column by name rather than assuming position, since
  // column_names order mirrors schema order but shouldn't be hardcoded.
  int name_idx = -1;
  for (size_t i = 0; i < result.column_names.size(); ++i) {
    if (result.column_names[i] == "name") name_idx = static_cast<int>(i);
  }
  CHECK(name_idx >= 0);
  CHECK(result.rows[0][static_cast<size_t>(name_idx)] == Value::Text("alice"));

  QueryResult all = engine.ExecuteSQL("SELECT * FROM users;");
  CHECK(all.rows.size() == 3);
}

void TestEndToEndJoin() {
  std::string db = TempDbPath("join");
  std::string wal = TempWalPath("join");
  RemoveIfExists(db);
  RemoveIfExists(wal);

  Engine engine(db, wal, 64);
  engine.ExecuteSQL("CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT);");
  engine.ExecuteSQL(
      "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER, "
      "amount INTEGER);");
  engine.ExecuteSQL("INSERT INTO customers VALUES (1, 'alice');");
  engine.ExecuteSQL("INSERT INTO customers VALUES (2, 'bob');");
  engine.ExecuteSQL("INSERT INTO orders VALUES (100, 1, 50);");
  engine.ExecuteSQL("INSERT INTO orders VALUES (101, 1, 75);");
  engine.ExecuteSQL("INSERT INTO orders VALUES (102, 2, 20);");

  QueryResult result = engine.ExecuteSQL(
      "SELECT customers.name, orders.amount FROM customers "
      "JOIN orders ON customers.id = orders.customer_id "
      "WHERE orders.amount > 30;");
  CHECK(result.rows.size() == 2);  // alice's two orders > 30; bob's is 20
}

void TestSemanticErrorsRejected() {
  std::string db = TempDbPath("semantic");
  std::string wal = TempWalPath("semantic");
  RemoveIfExists(db);
  RemoveIfExists(wal);

  Engine engine(db, wal, 64);
  engine.ExecuteSQL("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);");
  engine.ExecuteSQL("INSERT INTO t VALUES (1, 10);");

  // Unknown table.
  CHECK_THROWS(engine.ExecuteSQL("SELECT * FROM ghost;"));
  // Unknown column.
  CHECK_THROWS(engine.ExecuteSQL("SELECT nope FROM t;"));
  CHECK_THROWS(engine.ExecuteSQL("SELECT * FROM t WHERE nope = 1;"));
  // Wrong INSERT value count.
  CHECK_THROWS(engine.ExecuteSQL("INSERT INTO t VALUES (1);"));
  // INSERT naming an unknown column.
  CHECK_THROWS(engine.ExecuteSQL("INSERT INTO t (id, nope) VALUES (1, 2);"));
  // Duplicate alias.
  CHECK_THROWS(engine.ExecuteSQL("SELECT * FROM t AS a JOIN t AS a ON a.id = a.id;"));

  // All of the above must have left the table untouched: still exactly 1 row.
  QueryResult result = engine.ExecuteSQL("SELECT * FROM t;");
  CHECK(result.rows.size() == 1);
}

void TestCreateTableConstraintErrorsRejected() {
  std::string db = TempDbPath("create_constraints");
  std::string wal = TempWalPath("create_constraints");
  RemoveIfExists(db);
  RemoveIfExists(wal);

  Engine engine(db, wal, 64);
  CHECK_THROWS(
      engine.ExecuteSQL("CREATE TABLE no_pk (id INTEGER, v INTEGER);"));
  CHECK_THROWS(
      engine.ExecuteSQL("CREATE TABLE text_pk (id TEXT PRIMARY KEY);"));

  engine.ExecuteSQL("CREATE TABLE ok (id INTEGER PRIMARY KEY);");
  CHECK_THROWS(
      engine.ExecuteSQL("CREATE TABLE ok (id INTEGER PRIMARY KEY);"));
}

// The engine-level analogue of test_recovery.cpp: write data, destroy the
// Engine (simulating a clean process exit, no crash), reopen a fresh
// Engine over the same files, and confirm every table/row survived —
// proof that Engine's recovery sequencing (WAL truncate -> replay ->
// THEN construct Catalog/BufferPool) is wired correctly, the same
// invariant Database already guarantees for the single-table path.
void TestEngineReopenRecoversData() {
  std::string db = TempDbPath("reopen");
  std::string wal = TempWalPath("reopen");
  RemoveIfExists(db);
  RemoveIfExists(wal);

  {
    Engine engine(db, wal, 64);
    engine.ExecuteSQL(
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
    for (int i = 0; i < 20; ++i) {
      engine.ExecuteSQL("INSERT INTO users VALUES (" + std::to_string(i) +
                        ", 'u" + std::to_string(i) + "');");
    }
  }  // Engine destructs: no explicit flush call exists on Engine itself,
     // but every INSERT already went through BufferPool::UnpinPage's
     // WAL-fsync-before-return path (same durability guarantee as
     // milestone 2), so nothing here depends on a clean shutdown.

  {
    Engine engine(db, wal, 64);
    QueryResult result = engine.ExecuteSQL("SELECT * FROM users;");
    CHECK(result.rows.size() == 20);

    // The reopened engine's catalog/BTrees must still accept further
    // writes, not just reads.
    engine.ExecuteSQL("INSERT INTO users VALUES (20, 'u20');");
    QueryResult result2 = engine.ExecuteSQL("SELECT * FROM users;");
    CHECK(result2.rows.size() == 21);
  }
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    void (*fn)();
  };
  NamedTest tests[] = {
      {"TestEndToEndCreateInsertSelect", TestEndToEndCreateInsertSelect},
      {"TestEndToEndJoin", TestEndToEndJoin},
      {"TestSemanticErrorsRejected", TestSemanticErrorsRejected},
      {"TestCreateTableConstraintErrorsRejected",
       TestCreateTableConstraintErrorsRejected},
      {"TestEngineReopenRecoversData", TestEngineReopenRecoversData},
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
