#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast.h"
#include "row_codec.h"
#include "schema.h"
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

TableSchema MakeSchema() {
  TableSchema schema;
  schema.table_name = "t";
  schema.columns = {
      ColumnDef{"id", ColumnType::kInteger, true},
      ColumnDef{"name", ColumnType::kText, false},
      ColumnDef{"score", ColumnType::kInteger, false},
  };
  return schema;
}

void TestEncodeDecodeRoundTrip() {
  TableSchema schema = MakeSchema();
  RowCodec codec(schema);

  std::vector<Value> values = {Value::Int(7), Value::Text("hello"),
                                Value::Int(-42)};
  std::string blob = codec.Encode(values);
  CHECK(blob.size() == codec.EncodedSize());

  std::vector<Value> decoded = codec.Decode(7, blob);
  CHECK(decoded.size() == 3);
  CHECK(decoded[0] == Value::Int(7));   // substituted from the key, not blob
  CHECK(decoded[1] == Value::Text("hello"));
  CHECK(decoded[2] == Value::Int(-42));
}

void TestEmptyTextValue() {
  TableSchema schema = MakeSchema();
  RowCodec codec(schema);
  std::vector<Value> values = {Value::Int(1), Value::Text(""), Value::Int(0)};
  std::string blob = codec.Encode(values);
  auto decoded = codec.Decode(1, blob);
  CHECK(decoded[1] == Value::Text(""));
}

void TestWrongValueCountRejected() {
  TableSchema schema = MakeSchema();
  RowCodec codec(schema);
  CHECK_THROWS(codec.Encode({Value::Int(1)}));
}

void TestTypeMismatchRejected() {
  TableSchema schema = MakeSchema();
  RowCodec codec(schema);
  // "name" (TEXT) given an integer, "score" (INTEGER) given text.
  CHECK_THROWS(
      codec.Encode({Value::Int(1), Value::Int(5), Value::Int(0)}));
  CHECK_THROWS(codec.Encode(
      {Value::Int(1), Value::Text("ok"), Value::Text("nope")}));
}

void TestTextTooLongRejected() {
  TableSchema schema = MakeSchema();
  RowCodec codec(schema);
  // TEXT_COLUMN_CAPACITY is 16 (1 length byte + 15 content bytes).
  std::string too_long(20, 'x');
  CHECK_THROWS(
      codec.Encode({Value::Int(1), Value::Text(too_long), Value::Int(0)}));

  // Exactly at the limit must succeed.
  std::string max_len(15, 'y');
  std::string blob =
      codec.Encode({Value::Int(1), Value::Text(max_len), Value::Int(0)});
  auto decoded = codec.Decode(1, blob);
  CHECK(decoded[1] == Value::Text(max_len));
}

void TestEncodedSizeExcludesPrimaryKey() {
  TableSchema schema = MakeSchema();
  RowCodec codec(schema);
  // name (TEXT, 16 bytes) + score (INTEGER, 8 bytes) == 24. The primary
  // key ("id") is never encoded into the blob — it's already the BTree
  // key the row is stored under.
  CHECK(codec.EncodedSize() == 24);
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    void (*fn)();
  };
  NamedTest tests[] = {
      {"TestEncodeDecodeRoundTrip", TestEncodeDecodeRoundTrip},
      {"TestEmptyTextValue", TestEmptyTextValue},
      {"TestWrongValueCountRejected", TestWrongValueCountRejected},
      {"TestTypeMismatchRejected", TestTypeMismatchRejected},
      {"TestTextTooLongRejected", TestTextTooLongRejected},
      {"TestEncodedSizeExcludesPrimaryKey", TestEncodedSizeExcludesPrimaryKey},
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
