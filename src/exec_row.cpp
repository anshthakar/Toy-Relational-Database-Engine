#include "exec_row.h"

namespace reldb {

const Value& ExecRow::Find(const std::optional<std::string>& table,
                            const std::string& column) const {
  if (table.has_value()) {
    for (const Field& f : fields) {
      if (f.table == *table && f.column == column) {
        return f.value;
      }
    }
    throw ColumnResolutionError("no column '" + *table + "." + column +
                                 "' in this row");
  }

  const Value* found = nullptr;
  for (const Field& f : fields) {
    if (f.column == column) {
      if (found != nullptr) {
        throw ColumnResolutionError(
            "column '" + column +
            "' is ambiguous — it exists in more than one joined table; "
            "qualify it (e.g. table." +
            column + ")");
      }
      found = &f.value;
    }
  }
  if (found == nullptr) {
    throw ColumnResolutionError("no column '" + column + "' in this row");
  }
  return *found;
}

}  // namespace reldb
