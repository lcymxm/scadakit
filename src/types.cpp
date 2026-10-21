#include "scada/types.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace scada {

const char* to_string(Quality quality) {
    switch (quality) {
        case Quality::Good:
            return "good";
        case Quality::Uncertain:
            return "uncertain";
        case Quality::Bad:
            return "bad";
        case Quality::Stale:
            return "stale";
        case Quality::CommFailure:
            return "comm-failure";
    }
    return "?";
}

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string format_timestamp(std::int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    const int millis = static_cast<int>(timestamp_ms % 1000);

    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    std::ostringstream out;
    out << std::setfill('0') << std::setw(4) << (local.tm_year + 1900) << '-' << std::setw(2)
        << (local.tm_mon + 1) << '-' << std::setw(2) << local.tm_mday << ' ' << std::setw(2)
        << local.tm_hour << ':' << std::setw(2) << local.tm_min << ':' << std::setw(2)
        << local.tm_sec << '.' << std::setw(3) << millis;
    return out.str();
}

}  // namespace scada
