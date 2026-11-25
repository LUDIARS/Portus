#pragma once

#include <cstdint>
#include <string_view>

namespace portus::detail {

// Convert the IMDS expiration's UTC seconds to epoch milliseconds, or return 0 on invalid input.
std::int64_t parse_iso_time(std::string_view text);

}
