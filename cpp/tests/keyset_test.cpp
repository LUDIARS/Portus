#include "test.hpp"
#include "portus/keyset.hpp"
#include <limits>

TEST(keyset_lookahead, "Keep the last visible key for resuming a page") {
	auto page = portus::read_keyset_page<int>(2, 500, [](std::size_t count) {
		CHECK_EQ(count, 3u);
		return std::vector<int>{9, 7, 5};
	});
	CHECK(page.has_more);
	CHECK((page.rows == std::vector<int>{9, 7}));
}

TEST(keyset_final, "Empty, short and full final pages have no next cursor") {
	for (auto rows : {std::vector<int>{}, std::vector<int>{9}, std::vector<int>{9, 7}}) {
		auto page = portus::read_keyset_page<int>(2, 2, [&](std::size_t) { return rows; });
		CHECK(!page.has_more);
		CHECK_EQ(page.rows, rows);
	}
}

TEST(keyset_bounds, "Reject invalid sizes before reading, and reject an overproducing source") {
	unsigned calls = 0;
	auto fetch = [&](std::size_t) { ++calls; return std::vector<int>{1, 2, 3}; };
	CHECK_THROWS(portus::read_keyset_page<int>(0, 500, fetch));
	CHECK_THROWS(portus::read_keyset_page<int>(501, 500, fetch));
	const auto largest = (std::numeric_limits<std::size_t>::max)();
	CHECK_THROWS(portus::read_keyset_page<int>(largest, largest, fetch));
	CHECK_EQ(calls, 0u);
	CHECK_THROWS(portus::read_keyset_page<int>(1, 500, fetch));
	CHECK_EQ(calls, 1u);
	CHECK_EQ(portus::page_fetch_count(500, 500), 501u);
}
