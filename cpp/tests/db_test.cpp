#include "test.hpp"
#include "portus/db.hpp"
#include "portus/keyset.hpp"

namespace {
using namespace portus;
class Reader final : public DbConnection {
public:
    unsigned calls = 0;
    std::vector<DbText> fields{"task", std::nullopt, ""};
    bool disconnected = false;
    void begin(DbDeadline) override {}
    void commit(DbDeadline) override {}
    void rollback(DbDeadline) override {}
private:
    void execute(const DbQuery& query, DbDeadline, DbResult& result) override {
        ++calls;
        if (disconnected) throw DbDisconnected("disconnected");
        CHECK_EQ(query.params.at(0).value(), "team");
        result.append(fields);
        result.append(fields);
    }
};

TEST(db_page, "Read one visible row plus lookahead with distinct NULL and empty text") {
    Reader reader;
    auto page = read_keyset_page<DbRow>(1, 500, [&](std::size_t count) {
        return reader.query({"select ...", {"team"}, count}, DbDeadline::max());
    });
    CHECK(page.has_more);
    CHECK_EQ(page.rows.size(), 1u);
    CHECK_EQ(page.rows.at(0).at(0).value(), "task");
    CHECK(!page.rows.at(0).at(1));
    CHECK_EQ(page.rows.at(0).at(2).value(), "");
}

TEST(db_bounds, "Reject invalid input before driver entry and cap returned rows and fields") {
    Reader reader;
    const auto end = DbDeadline::max();
    CHECK_THROWS(reader.query({"", {}, 0}, end));
    CHECK_THROWS(reader.query({"select $1", {std::string_view("x\0y", 3)}, 1}, end));
    CHECK_THROWS(reader.query({"select $1", {"team"}, db_max_rows + 1}, end));
    CHECK_THROWS(reader.query({"select $1", {"team"}, 1}, DbDeadline::min()));
    CHECK_EQ(reader.calls, 0u);
    CHECK_THROWS(reader.query({"select $1", {"team"}, 1}, end));
    reader.fields.resize(db_max_columns + 1);
    CHECK_THROWS(reader.query({"select $1", {"team"}, 2}, end));
    const std::string oversized(db_max_value_bytes + 1, 'x');
    reader.fields = {oversized};
    CHECK_THROWS(reader.query({"select $1", {"team"}, 2}, end));
    const std::string full(db_max_value_bytes, 'x');
    reader.fields.assign(db_max_result_bytes / db_max_value_bytes, full);
    CHECK_THROWS(reader.query({"select $1", {"team"}, 2}, end));
}

TEST(db_disconnect, "Expose driver disconnect as a distinct failure") {
    Reader reader;
    reader.disconnected = true;
    bool caught = false;
    try { reader.query({"select $1", {"team"}, 2}, DbDeadline::max()); }
    catch (const DbDisconnected&) { caught = true; }
    CHECK(caught);
}
}
