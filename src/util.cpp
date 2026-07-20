#include "util.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace rudp {

std::string rfc3339_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t seconds = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << millis.count() << 'Z';
    return out.str();
}

long env_milliseconds(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0) {
        return fallback;
    }
    return parsed;
}

std::uint32_t random_u32() {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<std::uint32_t> distribution;
    return distribution(generator);
}

std::string errno_message(const std::string& prefix) {
    return prefix + ": " + std::strerror(errno);
}

} // namespace rudp
