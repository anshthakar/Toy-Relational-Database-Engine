#include "row_codec.h"

#include <cstring>

namespace reldb {

RowCodec::RowCodec(const TableSchema& schema)
    : schema_(schema), pk_index_(schema.PrimaryKeyIndex()) {
  size_t total = 0;
  for (size_t i = 0; i < schema_.columns.size(); ++i) {
    if (static_cast<int>(i) == pk_index_) continue;  // not encoded
    total += ColumnWidth(schema_.columns[i]);
  }
  encoded_size_ = total;
}

std::string RowCodec::Encode(const std::vector<Value>& values) const {
  if (values.size() != schema_.columns.size()) {
    throw RowEncodingError(
        "RowCodec::Encode: expected " +
        std::to_string(schema_.columns.size()) + " values, got " +
        std::to_string(values.size()));
  }

  std::string blob;
  blob.resize(encoded_size_);
  size_t offset = 0;

  for (size_t i = 0; i < schema_.columns.size(); ++i) {
    if (static_cast<int>(i) == pk_index_) continue;
    const ColumnDef& col = schema_.columns[i];
    const Value& val = values[i];

    if (col.type == ColumnType::kInteger) {
      if (val.kind != ValueKind::kInteger) {
        throw RowEncodingError("RowCodec::Encode: column '" + col.name +
                                "' expects an INTEGER value");
      }
      std::memcpy(&blob[offset], &val.int_value, sizeof(int64_t));
      offset += sizeof(int64_t);
    } else {
      if (val.kind != ValueKind::kText) {
        throw RowEncodingError("RowCodec::Encode: column '" + col.name +
                                "' expects a TEXT value");
      }
      // 1-byte length prefix + up to (TEXT_COLUMN_CAPACITY - 1) content
      // bytes. A value longer than that genuinely cannot be represented
      // under this fixed-width scheme — rejected outright rather than
      // silently truncated, which would corrupt data without any error.
      if (val.text_value.size() > TEXT_COLUMN_CAPACITY - 1) {
        throw RowEncodingError(
            "RowCodec::Encode: value for TEXT column '" + col.name +
            "' is " + std::to_string(val.text_value.size()) +
            " bytes, exceeding the " +
            std::to_string(TEXT_COLUMN_CAPACITY - 1) +
            "-byte limit for this column");
      }
      blob[offset] = static_cast<char>(val.text_value.size());
      std::memcpy(&blob[offset + 1], val.text_value.data(),
                  val.text_value.size());
      // Remaining bytes up to TEXT_COLUMN_CAPACITY stay whatever
      // std::string::resize() zero-initialized them to above — never
      // read back (Decode() only reads the length-prefixed portion), but
      // leaving them zeroed keeps the stored page bytes deterministic,
      // the same reasoning milestone 2's crash-recovery testing depended
      // on for leaf cells generally.
      offset += TEXT_COLUMN_CAPACITY;
    }
  }

  return blob;
}

std::vector<Value> RowCodec::Decode(int64_t key, const std::string& blob) const {
  std::vector<Value> values(schema_.columns.size());
  size_t offset = 0;
  for (size_t i = 0; i < schema_.columns.size(); ++i) {
    if (static_cast<int>(i) == pk_index_) {
      values[i] = Value::Int(key);
      continue;
    }
    const ColumnDef& col = schema_.columns[i];
    if (col.type == ColumnType::kInteger) {
      int64_t v = 0;
      std::memcpy(&v, &blob[offset], sizeof(int64_t));
      values[i] = Value::Int(v);
      offset += sizeof(int64_t);
    } else {
      uint8_t len = static_cast<uint8_t>(blob[offset]);
      values[i] = Value::Text(blob.substr(offset + 1, len));
      offset += TEXT_COLUMN_CAPACITY;
    }
  }
  return values;
}

}  // namespace reldb
