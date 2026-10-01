#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "ast.h"
#include "buffer_pool.h"
#include "catalog.h"
#include "disk_manager.h"
#include "logical_plan.h"
#include "parser.h"
#include "physical_plan.h"
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

namespace {

std::string TempDbPath(const std::string& name) {
  return "/tmp/reldb_planner_test_" + name + ".db";
}

void RemoveIfExists(const std::string& path) { unlink(path.c_str()); }

std::unique_ptr<SelectStmt> ParseSelect(const std::string& sql) {
  std::unique_ptr<Statement> stmt = ParseSQL(sql);
  if (stmt->kind != StatementKind::kSelect) {
    throw std::runtime_error("ParseSelect: expected a SELECT statement");
  }
  return std::unique_ptr<SelectStmt>(static_cast<SelectStmt*>(stmt.release()));
}

std::vector<ExecRow> RunPlan(PhysicalPlan& plan) {
  plan.Open();
  std::vector<ExecRow> rows;
  std::optional<ExecRow> r;
  while ((r = plan.Next()).has_value()) {
    rows.push_back(std::move(*r));
  }
  plan.Close();
  return rows;
}

const Value& FieldValue(const ExecRow& row, const std::string& table,
                        const std::string& column) {
  return row.Find(table, column);
}

// ---------- Tests ----------

// The cost estimator must pick hash join when it's cheaper: two
// reasonably large equi-joined tables, where left*right (nested-loop)
// dwarfs left+right (hash join).
void TestCostEstimatorChoosesHashJoinForLargeTables() {
  std::string path = TempDbPath("hash_choice");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(256, &dm);
  Catalog cat(&bp);

  cat.CreateTable("customers",
                   {ColumnDef{"id", ColumnType::kInteger, true},
                    ColumnDef{"name", ColumnType::kText, false}});
  cat.CreateTable("orders",
                   {ColumnDef{"id", ColumnType::kInteger, true},
                    ColumnDef{"customer_id", ColumnType::kInteger, false}});

  for (int i = 0; i < 50; ++i) {
    cat.InsertRow("customers",
                  {Value::Int(i), Value::Text("c" + std::to_string(i))});
    cat.InsertRow("orders", {Value::Int(i), Value::Int(i)});
  }

  auto stmt = ParseSelect(
      "SELECT * FROM orders JOIN customers ON orders.customer_id = "
      "customers.id;");
  auto logical = BuildLogicalPlan(*stmt);
  auto physical = BuildPhysicalPlan(*logical, &cat);

  // physical == Project(JoinNode) — no WHERE in this query.
  const auto* project = dynamic_cast<const PhysicalProject*>(physical.get());
  CHECK(project != nullptr);
  const auto* hash_join =
      dynamic_cast<const PhysicalHashJoin*>(project->input());
  CHECK(hash_join != nullptr);  // the cost model must have picked hash join

  auto rows = RunPlan(*physical);
  CHECK(rows.size() == 50);  // every order matches exactly one customer
}

// With tiny tables, nested_cost (L*R) can beat hash_cost (L+R) — here
// 1*3 == 3 < 1+3 == 4 — so the estimator should choose nested-loop even
// though the join is a plain equality.
void TestCostEstimatorChoosesNestedLoopForTinyTables() {
  std::string path = TempDbPath("nested_choice");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  Catalog cat(&bp);

  cat.CreateTable("a", {ColumnDef{"id", ColumnType::kInteger, true},
                        ColumnDef{"tag", ColumnType::kInteger, false}});
  cat.CreateTable("b", {ColumnDef{"id", ColumnType::kInteger, true},
                        ColumnDef{"a_tag", ColumnType::kInteger, false}});

  cat.InsertRow("a", {Value::Int(1), Value::Int(100)});
  cat.InsertRow("b", {Value::Int(1), Value::Int(100)});
  cat.InsertRow("b", {Value::Int(2), Value::Int(999)});
  cat.InsertRow("b", {Value::Int(3), Value::Int(999)});

  auto stmt =
      ParseSelect("SELECT * FROM a JOIN b ON a.tag = b.a_tag;");
  auto logical = BuildLogicalPlan(*stmt);
  auto physical = BuildPhysicalPlan(*logical, &cat);

  const auto* project = dynamic_cast<const PhysicalProject*>(physical.get());
  CHECK(project != nullptr);
  const auto* nested =
      dynamic_cast<const PhysicalNestedLoopJoin*>(project->input());
  CHECK(nested != nullptr);  // the cost model must have picked nested-loop

  auto rows = RunPlan(*physical);
  CHECK(rows.size() == 1);  // only b.id=1 matches a.tag=100
}

// A non-equi join condition (a range comparison) can never use hash
// join, regardless of table sizes — this is a correctness requirement,
// not just a cost preference.
void TestNonEquiJoinAlwaysUsesNestedLoop() {
  std::string path = TempDbPath("non_equi");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(256, &dm);
  Catalog cat(&bp);

  cat.CreateTable("a", {ColumnDef{"id", ColumnType::kInteger, true},
                        ColumnDef{"v", ColumnType::kInteger, false}});
  cat.CreateTable("b", {ColumnDef{"id", ColumnType::kInteger, true},
                        ColumnDef{"v", ColumnType::kInteger, false}});
  for (int i = 0; i < 40; ++i) {
    cat.InsertRow("a", {Value::Int(i), Value::Int(i)});
    cat.InsertRow("b", {Value::Int(i), Value::Int(i)});
  }

  auto stmt = ParseSelect("SELECT * FROM a JOIN b ON a.v < b.v;");
  auto logical = BuildLogicalPlan(*stmt);
  auto physical = BuildPhysicalPlan(*logical, &cat);

  const auto* project = dynamic_cast<const PhysicalProject*>(physical.get());
  CHECK(project != nullptr);
  const auto* nested =
      dynamic_cast<const PhysicalNestedLoopJoin*>(project->input());
  CHECK(nested != nullptr);

  auto rows = RunPlan(*physical);
  // Number of (i, j) pairs with i < j, i,j in [0,40): 40*39/2 = 780.
  CHECK(rows.size() == 780);
}

void TestThreeTableJoinCorrectness() {
  std::string path = TempDbPath("three_table");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(256, &dm);
  Catalog cat(&bp);

  cat.CreateTable("customers",
                   {ColumnDef{"id", ColumnType::kInteger, true},
                    ColumnDef{"name", ColumnType::kText, false}});
  cat.CreateTable("orders",
                   {ColumnDef{"id", ColumnType::kInteger, true},
                    ColumnDef{"customer_id", ColumnType::kInteger, false}});
  cat.CreateTable("items",
                   {ColumnDef{"id", ColumnType::kInteger, true},
                    ColumnDef{"order_id", ColumnType::kInteger, false},
                    ColumnDef{"qty", ColumnType::kInteger, false}});

  cat.InsertRow("customers", {Value::Int(1), Value::Text("alice")});
  cat.InsertRow("customers", {Value::Int(2), Value::Text("bob")});
  cat.InsertRow("orders", {Value::Int(10), Value::Int(1)});
  cat.InsertRow("orders", {Value::Int(11), Value::Int(2)});
  cat.InsertRow("items", {Value::Int(100), Value::Int(10), Value::Int(3)});
  cat.InsertRow("items", {Value::Int(101), Value::Int(10), Value::Int(5)});
  cat.InsertRow("items", {Value::Int(102), Value::Int(11), Value::Int(1)});

  auto stmt = ParseSelect(
      "SELECT customers.name, items.qty FROM customers "
      "JOIN orders ON customers.id = orders.customer_id "
      "JOIN items ON orders.id = items.order_id;");
  auto logical = BuildLogicalPlan(*stmt);
  auto physical = BuildPhysicalPlan(*logical, &cat);

  auto rows = RunPlan(*physical);
  CHECK(rows.size() == 3);  // alice has 2 items, bob has 1

  int alice_qty_sum = 0;
  int bob_qty_sum = 0;
  for (const auto& row : rows) {
    CHECK(row.fields.size() == 2);
    const Value& name = row.fields[0].value;
    const Value& qty = row.fields[1].value;
    if (name == Value::Text("alice")) {
      alice_qty_sum += static_cast<int>(qty.int_value);
    } else if (name == Value::Text("bob")) {
      bob_qty_sum += static_cast<int>(qty.int_value);
    } else {
      CHECK(false);  // unexpected customer name
    }
  }
  CHECK(alice_qty_sum == 8);  // 3 + 5
  CHECK(bob_qty_sum == 1);
}

void TestWhereFiltersAfterJoin() {
  std::string path = TempDbPath("where_after_join");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  Catalog cat(&bp);

  cat.CreateTable("a", {ColumnDef{"id", ColumnType::kInteger, true},
                        ColumnDef{"v", ColumnType::kInteger, false}});
  cat.CreateTable("b", {ColumnDef{"id", ColumnType::kInteger, true},
                        ColumnDef{"a_id", ColumnType::kInteger, false}});
  cat.InsertRow("a", {Value::Int(1), Value::Int(10)});
  cat.InsertRow("a", {Value::Int(2), Value::Int(20)});
  cat.InsertRow("b", {Value::Int(1), Value::Int(1)});
  cat.InsertRow("b", {Value::Int(2), Value::Int(2)});

  auto stmt = ParseSelect(
      "SELECT * FROM a JOIN b ON a.id = b.a_id WHERE a.v = 20;");
  auto logical = BuildLogicalPlan(*stmt);
  auto physical = BuildPhysicalPlan(*logical, &cat);

  const auto* project = dynamic_cast<const PhysicalProject*>(physical.get());
  CHECK(project != nullptr);
  const auto* filter = dynamic_cast<const PhysicalFilter*>(project->input());
  CHECK(filter != nullptr);  // WHERE must sit above the join

  auto rows = RunPlan(*physical);
  CHECK(rows.size() == 1);
  CHECK(FieldValue(rows[0], "a", "v") == Value::Int(20));
}

void TestProjectionSelectsNamedColumnsWithAlias() {
  std::string path = TempDbPath("projection");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  Catalog cat(&bp);

  cat.CreateTable("users",
                   {ColumnDef{"id", ColumnType::kInteger, true},
                    ColumnDef{"name", ColumnType::kText, false},
                    ColumnDef{"age", ColumnType::kInteger, false}});
  cat.InsertRow("users", {Value::Int(1), Value::Text("alice"), Value::Int(30)});

  auto stmt = ParseSelect("SELECT name AS n FROM users;");
  auto logical = BuildLogicalPlan(*stmt);
  auto physical = BuildPhysicalPlan(*logical, &cat);

  auto rows = RunPlan(*physical);
  CHECK(rows.size() == 1);
  CHECK(rows[0].fields.size() == 1);
  CHECK(rows[0].fields[0].column == "n");
  CHECK(rows[0].fields[0].value == Value::Text("alice"));
}

void TestSingleTableNoJoin() {
  std::string path = TempDbPath("single_table");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  Catalog cat(&bp);

  cat.CreateTable("t", {ColumnDef{"id", ColumnType::kInteger, true},
                        ColumnDef{"v", ColumnType::kInteger, false}});
  cat.InsertRow("t", {Value::Int(1), Value::Int(10)});
  cat.InsertRow("t", {Value::Int(2), Value::Int(20)});

  auto stmt = ParseSelect("SELECT * FROM t WHERE v = 20;");
  auto logical = BuildLogicalPlan(*stmt);
  auto physical = BuildPhysicalPlan(*logical, &cat);
  auto rows = RunPlan(*physical);
  CHECK(rows.size() == 1);
  CHECK(FieldValue(rows[0], "t", "id") == Value::Int(2));
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    void (*fn)();
  };
  NamedTest tests[] = {
      {"TestCostEstimatorChoosesHashJoinForLargeTables",
       TestCostEstimatorChoosesHashJoinForLargeTables},
      {"TestCostEstimatorChoosesNestedLoopForTinyTables",
       TestCostEstimatorChoosesNestedLoopForTinyTables},
      {"TestNonEquiJoinAlwaysUsesNestedLoop",
       TestNonEquiJoinAlwaysUsesNestedLoop},
      {"TestThreeTableJoinCorrectness", TestThreeTableJoinCorrectness},
      {"TestWhereFiltersAfterJoin", TestWhereFiltersAfterJoin},
      {"TestProjectionSelectsNamedColumnsWithAlias",
       TestProjectionSelectsNamedColumnsWithAlias},
      {"TestSingleTableNoJoin", TestSingleTableNoJoin},
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
