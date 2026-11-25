#pragma once
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace portus {

using DbDeadline = std::chrono::steady_clock::time_point;
using DbText = std::optional<std::string_view>;
using DbRow = std::vector<std::optional<std::string>>;
inline constexpr std::size_t db_max_sql_bytes = 64 * 1024;
inline constexpr std::size_t db_max_params = 64;
inline constexpr std::size_t db_max_value_bytes = 1024 * 1024;
inline constexpr std::size_t db_max_rows = 1024;
inline constexpr std::size_t db_max_columns = 128;
inline constexpr std::size_t db_max_result_bytes = 16 * 1024 * 1024;

// Borrowed text and parameters are valid for the synchronous call only.
struct DbQuery {
    std::string_view sql;
    std::vector<DbText> params;
    std::size_t max_rows;
};

// The pool wait, connect or statement exceeded its deadline.
class DbTimeout : public std::runtime_error { using std::runtime_error::runtime_error; };
// Transport loss can leave the outcome of a write unknown.
class DbDisconnected : public std::runtime_error { using std::runtime_error::runtime_error; };

// Query-scoped destination for driver rows. Never retain it after execute returns.
class DbResult {
public:
    // Copy one row while the driver's column buffers are still valid.
    void append(const std::vector<DbText>& fields);
    DbResult(const DbResult&) = delete;
    DbResult& operator=(const DbResult&) = delete;
private:
    friend class DbConnection;
    explicit DbResult(std::size_t limit) : limit_(limit) {}
    std::size_t limit_;
    std::size_t bytes_ = 0;
    std::vector<DbRow> rows_;
};

// Exclusive worker-side lease. Destruction returns a clean session or discards it.
class DbConnection {
public:
    virtual ~DbConnection() = default;
    // Bind nullable UTF-8 text parameters and return owned rows.
    std::vector<DbRow> query(const DbQuery& request, DbDeadline deadline);
    // Start a transaction on this lease's physical connection.
    virtual void begin(DbDeadline deadline) = 0;
    // Confirm the transaction, propagating any uncertain outcome.
    virtual void commit(DbDeadline deadline) = 0;
    // Abort the transaction or invalidate the session if recovery fails.
    virtual void rollback(DbDeadline deadline) = 0;
private:
    virtual void execute(const DbQuery& request, DbDeadline deadline, DbResult& result) = 0;
};

// Driver-owned pool shared by DB workers.
class DbPool {
public:
    virtual ~DbPool() = default;
    // Return a non-null exclusive lease or throw within the time budget.
    virtual std::unique_ptr<DbConnection> acquire(DbDeadline deadline) = 0;
};
}
