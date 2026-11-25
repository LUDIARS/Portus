#include "portus/keyset.hpp"
#include <limits>
#include "augur/contract_runtime.hpp" /* augur-inject:import:77be2cec */
#include "page_count.contract.hpp" /* augur-inject:contract-predicate:6cce93a5 */

namespace portus {

std::size_t page_fetch_count(std::size_t limit, std::size_t maximum) { return ::augur::invoke_observed(::augur::ContractSpec{"C-1", "d3cb1519", "cpp/src/paging/keyset.cpp:6"}, contracts::page_count_checks, [&]() -> std::size_t { /* augur-inject:contract-wrap:d3cb1519 */
	if (limit == 0 || limit > maximum || limit == (std::numeric_limits<std::size_t>::max)()) return 0;
	return limit + 1;
}, limit, maximum); /* augur-inject:contract-end:d3cb1519 */ }

}
