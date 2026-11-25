#pragma once

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace portus {

// Returns zero if the requested page cannot be fetched with one lookahead row.
std::size_t page_fetch_count(std::size_t limit, std::size_t maximum);

template<class Row>
struct KeysetPage {
	std::vector<Row> rows;
	bool has_more;
};

// fetch(count) returns at most count authorized rows in stable keyset order.
// The caller encodes the last visible row when has_more is true.
template<class Row, class Fetch>
KeysetPage<Row> read_keyset_page(std::size_t limit, std::size_t maximum, Fetch&& fetch) {
	const auto count = page_fetch_count(limit, maximum);
	if (count == 0) throw std::invalid_argument("invalid page limit");
	KeysetPage<Row> page{fetch(count), false};
	if (page.rows.size() > count) throw std::length_error("page source exceeded requested count");
	if (page.rows.size() == count) {
		page.rows.pop_back();
		page.has_more = true;
	}
	return page;
}

}
