#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "schema.h"
#include "value.h"

namespace reldb {

// Thrown for anything that makes a row impossible to store under this
// codec's fixed layout: a schema with too many/too-wide columns for
// VALUE_SIZE, or a TEXT value longer than TEXT_COLUMN_CAPACITY allows.
struct RowEncodingError : std::runtime_error {
  explicit RowEncodingError(const std::string& msg) : std::runtime_error(msg) {}
};

// A TEXT column's stored capacity: 1 length byte + 15 data bytes, chosen
// so several such columns still fit comfortably inside VALUE_SIZE (120
// bytes — see btree_node.h). A real engine would use the page's own
// variable-length slotted layout for this; this project's B+tree nodes
// are fixed-stride cells (milestone 1's documented simplification), so
// every column width has to be fixed too, all the way down.
constexpr size_t TEXT_COLUMN_CAPACITY = 16;

// Fixed-width-per-column encoding of a row's NON-primary-key column
// values into a single blob — this is exactly what gets stored as a
// B+tree LeafCell's `value` (see btree_node.h). The primary key column's
// value is never encoded here: it's already the BTree key the row is
// stored under, so encoding it a second time into the value would be
// pure waste. INTEGER columns take a fixed 8 bytes; TEXT columns take
// TEXT_COLUMN_CAPACITY bytes (a 1-byte length prefix + up to 15 content
// bytes — a TEXT value longer than that is a RowEncodingError, not a
// silent truncation).
class RowCodec {
 public:
  explicit RowCodec(const TableSchema& schema);

  // Total bytes Encode() will produce — checked by the caller (Catalog)
  // against the B+tree's VALUE_SIZE at CREATE TABLE time, so a schema
  // that can never fit a row fails immediately and clearly instead of
  // on the first INSERT.
  size_t EncodedSize() const { return encoded_size_; }

  // `values` must have exactly schema_.columns.size() entries, in schema
  // column order, INCLUDING the primary key's own value at its column
  // index (RowCodec reads every entry EXCEPT the one at
  // schema_.PrimaryKeyIndex(), which the caller already has separately
  // as the BTree key).
  std::string Encode(const std::vector<Value>& values) const;

  // Reconstructs every column's value in schema order, substituting
  // `key` back in at the primary key's position — the inverse of Encode,
  // given the key that was stored alongside `blob`.
  std::vector<Value> Decode(int64_t key, const std::string& blob) const;

 private:
  size_t ColumnWidth(const ColumnDef& col) const {
    return col.type == ColumnType::kInteger ? sizeof(int64_t)
                                             : TEXT_COLUMN_CAPACITY;
  }

  const TableSchema& schema_;
  int pk_index_;
  size_t encoded_size_;
};

}  // namespace reldb
