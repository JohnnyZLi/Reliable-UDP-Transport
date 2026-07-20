#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace rudp {

std::string rfc3339_now();
long env_milliseconds(const char* name, long fallback);
std::uint32_t random_u32();
std::string errno_message(const std::string& prefix);

} // namespace rudp
