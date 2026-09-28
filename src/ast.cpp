#include "ast.h"

namespace reldb {

    const char* ColumnTypeName(ColumnType type) {
        switch (type) {
            case ColumnType::kInteger: return "INTEGER";
            case ColumnType::kText: return "TEXT";
        }
        return "<unknown type>";
    }

}  // namespace reldb