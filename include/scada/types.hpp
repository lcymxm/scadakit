// Small value types shared by every module: a sample's quality code is as much
// part of a SCADA value as the number itself.
#pragma once

#include <cstdint>
#include <string>

namespace scada {

// Quality follows the shape of OPC-style quality codes: the operator has to be
// able to tell "the value really is 0" apart from "we could not read the device".
enum class Quality {
    Good = 0,      // freshly read from the device
    Uncertain,     // read, but the device reported a problem
    Bad,           // value exists but is not usable
    Stale,         // not refreshed for longer than the configured timeout
    CommFailure,   // the device is not answering at all
};

const char* to_string(Quality quality);

inline bool is_usable(Quality quality) {
    return quality == Quality::Good || quality == Quality::Uncertain;
}

// Milliseconds since the Unix epoch.
std::int64_t now_ms();

// ISO-8601 local timestamp with millisecond resolution, used in logs and CSV.
std::string format_timestamp(std::int64_t timestamp_ms);

}  // namespace scada
