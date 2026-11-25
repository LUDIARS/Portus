#pragma once
#include "augur/contract_runtime.hpp"
#include "portus/db.hpp"
#include <algorithm>

namespace contracts {
inline const auto db_query_checks = augur::predicates(
    [](const portus::DbQuery&) { return true; },
    [](bool result, const portus::DbQuery& query) {
        const bool valid = !query.sql.empty() && query.sql.size() <= portus::db_max_sql_bytes
            && query.sql.find('\0') == std::string_view::npos
            && query.params.size() <= portus::db_max_params && query.max_rows <= portus::db_max_rows
            && std::all_of(query.params.begin(), query.params.end(), [](const auto& param) {
                return !param || (param->size() <= portus::db_max_value_bytes
                    && param->find('\0') == std::string_view::npos);
            });
        return result == valid;
    });
}
