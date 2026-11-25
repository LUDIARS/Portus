#include "portus/db.hpp"
#include <utility>
#include "augur/contract_runtime.hpp" /* augur-inject:import:da457091 */
#include "db_query.contract.hpp" /* augur-inject:contract-predicate:657f7407 */

namespace portus {

bool valid_db_query(const DbQuery& query) { return ::augur::invoke_observed(::augur::ContractSpec{"C-2", "7dd8bbbf", "cpp/src/paging/db.cpp:6"}, contracts::db_query_checks, [&]() -> bool { /* augur-inject:contract-wrap:7dd8bbbf */
    if (query.sql.empty() || query.sql.size() > db_max_sql_bytes ||
        query.sql.find('\0') != std::string_view::npos ||
        query.params.size() > db_max_params || query.max_rows > db_max_rows) return false;
    for (const auto& param : query.params) {
        if (param && (param->size() > db_max_value_bytes ||
            param->find('\0') != std::string_view::npos)) return false;
    }
    return true;
}, query); /* augur-inject:contract-end:7dd8bbbf */ }

std::vector<DbRow> DbConnection::query(const DbQuery& request, DbDeadline deadline) {
    if (!valid_db_query(request)) throw std::invalid_argument("invalid DB query");
    if (deadline <= std::chrono::steady_clock::now()) throw DbTimeout("DB deadline expired");
    DbResult result(request.max_rows);
    execute(request, deadline, result);
    return std::move(result.rows_);
}

// Validate the entire row before allocation. Commit its byte count only after insertion succeeds.
void DbResult::append(const std::vector<DbText>& fields) {
    if (rows_.size() >= limit_ || fields.size() > db_max_columns)
        throw std::length_error("DB result dimensions exceeded");
    auto total = bytes_;
    for (const auto& field : fields) {
        if (!field) continue;
        if (field->size() > db_max_value_bytes || field->size() > db_max_result_bytes - total)
            throw std::length_error("DB result byte limit exceeded");
        total += field->size();
    }
    DbRow row;
    row.reserve(fields.size());
    for (const auto& field : fields) row.emplace_back(field);
    rows_.push_back(std::move(row));
    bytes_ = total;
}
}
