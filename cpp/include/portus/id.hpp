#pragma once
#include <string>

namespace portus {

//UUID v4 (乱数は OpenSSL の CSPRNG)
std::string uuidv4();

}
