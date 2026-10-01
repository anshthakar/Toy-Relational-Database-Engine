#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace reldb {

// Runtime value flowing through execution — the Literal/ColumnRef in
// ast.h are the SQL-text-level representation (what the parser produced);
// Value is the same two-kind (integer/text) data after it's been read out
// of storage or evaluated from an expression, kept separate so execution
// code doesn't reach back into AST types for something that isn't
// SQL syntax anymore.
enum class ValueKind { kInteger, kText };

struct Value {
  ValueKind kind;
  int64_t int_value = 0;
  std::string text_value;

  static Value Int(int64_t v) {
    Value val;
    val.kind = ValueKind::kInteger;
    val.int_value = v;
    return val;
  }
  static Value Text(std::string v) {
    Value val;
    val.kind = ValueKind::kText;
    val.text_value = std::move(v);
    return val;
  }

  bool operator==(const Value& other) const {
    if (kind != other.kind) return false;
    return kind == ValueKind::kInteger ? int_value == other.int_value
                                        : text_value == other.text_value;
  }
};

// Needed by PhysicalHashJoin's build-side hash table (see physical_plan.h)
// — hashes on whichever member is actually meaningful for this value's
// kind, same split `operator==` makes.
struct ValueHash {
  size_t operator()(const Value& v) const {
    return v.kind == ValueKind::kInteger
               ? std::hash<int64_t>()(v.int_value)
               : std::hash<std::string>()(v.text_value);
  }
};

}  // namespace reldb
