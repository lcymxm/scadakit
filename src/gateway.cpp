#include "scada/gateway.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace scada {
namespace {

// Reads a run of registers/bits out of `block` for the tag at `index`.
double extract_value(const TagDefinition& tag, const PollBlock& block,
                     const modbus::ReadResponse& response) {
    const std::size_t relative = tag.address.offset - block.start;
    if (tag.address.type == DataType::Bit) {
        if (relative >= response.bits.size()) {
            return 0.0;
        }
        return decode_bit(response.bits[relative]) * tag.gain + tag.offset;
    }
    const std::uint16_t needed = register_count(tag.address.type);
    if (relative + needed > response.registers.size()) {
        return 0.0;
    }
    return to_engineering(tag, decode_raw(tag.address, response.registers.data() + relative, needed));
}

}  // namespace

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string json_number(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

std::vector<PollBlock> plan_blocks(const std::vector<TagDefinition>& tags,
                                   std::uint16_t max_gap) {
    struct Keyed {
        std::size_t index;
        std::uint16_t start;
        std::uint16_t end;
    };

    std::vector<PollBlock> blocks;
    // Group by station and by table, because a single Modbus request can only
    // touch one table.
    for (int station : [&tags]() {
             std::vector<int> seen;
             for (const TagDefinition& tag : tags) {
                 if (std::find(seen.begin(), seen.end(), static_cast<int>(tag.station)) ==
                     seen.end()) {
                     seen.push_back(static_cast<int>(tag.station));
                 }
             }
             std::sort(seen.begin(), seen.end());
             return seen;
         }()) {
        for (int kind_index = 0; kind_index < 4; ++kind_index) {
            const RegisterKind kind = static_cast<RegisterKind>(kind_index);
            std::vector<Keyed> members;
            for (std::size_t i = 0; i < tags.size(); ++i) {
                const TagDefinition& tag = tags[i];
                if (static_cast<int>(tag.station) != station || tag.address.kind != kind) {
                    continue;
                }
                const std::uint16_t span = register_count(tag.address.type);
                members.push_back(Keyed{i, tag.address.offset,
                                        static_cast<std::uint16_t>(tag.address.offset + span)});
            }
            if (members.empty()) {
                continue;
            }
            std::sort(members.begin(), members.end(),
                      [](const Keyed& a, const Keyed& b) { return a.start < b.start; });

            // `count` is a length, not an end offset. Conflating the two makes a
            // block that starts at register 8 ask the device for 10 registers.
            std::uint16_t block_start = members.front().start;
            std::uint16_t block_end = members.front().end;
            PollBlock current;
            current.station = static_cast<std::uint8_t>(station);
            current.kind = kind;
            current.tags.push_back(members.front().index);

            const auto flush = [&blocks](PollBlock& block, std::uint16_t start,
                                         std::uint16_t end) {
                block.start = start;
                block.count = static_cast<std::uint16_t>(end - start);
                blocks.push_back(block);
            };

            for (std::size_t m = 1; m < members.size(); ++m) {
                const Keyed& next = members[m];
                const std::uint16_t proposed_end = std::max(block_end, next.end);
                const std::uint32_t span = proposed_end - block_start;
                const bool close_enough = next.start <= block_end + max_gap;
                // 125 registers is the protocol limit for one read.
                const bool fits = span <= modbus::kMaxReadRegisters;
                if (close_enough && fits) {
                    block_end = proposed_end;
                    current.tags.push_back(next.index);
                } else {
                    flush(current, block_start, block_end);
                    current = PollBlock{};
                    current.station = static_cast<std::uint8_t>(station);
                    current.kind = kind;
                    current.tags.push_back(next.index);
                    block_start = next.start;
                    block_end = next.end;
                }
            }
            flush(current, block_start, block_end);
        }
    }
    return blocks;
}

Gateway::Gateway(TagTable table, GatewayConfig config)
    : table_(std::move(table)),
      config_(config),
      database_(table_),
      alarms_(table_),
      history_(table_.size(), config.history_capacity),
      blocks_(plan_blocks(table_.tags())),
      master_(config.master),
      last_history_ms_(table_.size(), 0) {}

void Gateway::mark_station_failed(std::uint8_t station) {
    for (std::size_t i = 0; i < table_.size(); ++i) {
        if (table_.tags()[i].station == station) {
            database_.set_quality(i, Quality::CommFailure);
        }
    }
}

void Gateway::age_out(std::int64_t now_ms) {
    for (std::size_t i = 0; i < database_.size(); ++i) {
        const TagValue& value = database_.value(i);
        if (!value.initialized) {
            continue;
        }
        if (!is_usable(value.quality)) {
            continue;
        }
        if (now_ms - value.timestamp_ms > config_.stale_after_ms) {
            database_.set_quality(i, Quality::Stale);
        }
    }
}

void Gateway::poll_once(std::int64_t now_ms) {
    // Phase 1 -- talk to the device. This can block for a whole timeout when a
    // station is dead, so it must not hold the lock the HMI needs.
    struct BlockResult {
        std::size_t index = 0;
        bool ok = false;
        bool is_exception = false;
        modbus::Exception exception = modbus::Exception::None;
        std::string error;
        modbus::ReadResponse response;
    };

    std::vector<BlockResult> results;
    results.reserve(blocks_.size());
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        BlockResult result;
        result.index = i;
        if (master_.read(function_for_kind(blocks_[i].kind), blocks_[i].start, blocks_[i].count,
                         result.response, result.error)) {
            if (result.response.is_exception) {
                result.is_exception = true;
                result.exception = result.response.exception;
            } else {
                result.ok = true;
            }
        }
        results.push_back(std::move(result));
    }

    // Phase 2 -- publish the samples.
    const std::lock_guard<std::mutex> guard(mutex_);
    ++poll_count_;
    bool any_success = false;
    std::string last_error;

    for (const BlockResult& result : results) {
        const PollBlock& block = blocks_[result.index];
        if (!result.ok) {
            last_error = result.is_exception
                             ? std::string("device exception: ") +
                                   modbus::to_string(result.exception)
                             : result.error;
            continue;
        }
        any_success = true;
        for (const std::size_t index : block.tags) {
            const TagDefinition& tag = table_.tags()[index];
            const double value = extract_value(tag, block, result.response);
            const bool stored = database_.update(index, value, Quality::Good, now_ms);
            alarms_.evaluate(index, value, Quality::Good, now_ms);
            const bool due = now_ms - last_history_ms_[index] >= config_.history_interval_ms;
            if (stored || due) {
                history_.append(index, now_ms, value, Quality::Good);
                last_history_ms_[index] = now_ms;
            }
        }
    }

    if (!any_success) {
        for (const PollBlock& block : blocks_) {
            mark_station_failed(block.station);
        }
    }
    device_online_ = any_success;
    if (!last_error.empty()) {
        last_error_ = last_error;
    } else if (any_success) {
        last_error_.clear();
    }
    age_out(now_ms);
}

std::size_t Gateway::acknowledge(const std::string& tag) {
    const std::lock_guard<std::mutex> guard(mutex_);
    return alarms_.acknowledge(tag, now_ms());
}

std::string Gateway::snapshot_json() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    const std::int64_t now = now_ms();
    std::ostringstream out;
    out << "{\"time\":" << now << ",\"time_text\":\"" << json_escape(format_timestamp(now))
        << "\",\"online\":" << (device_online_ ? "true" : "false")
        << ",\"polls\":" << poll_count_
        << ",\"unacked\":" << alarms_.unacknowledged_count()
        << ",\"highest\":" << alarms_.highest_active_level()
        << ",\"error\":\"" << json_escape(last_error_) << "\",\"tags\":[";

    for (std::size_t i = 0; i < database_.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        const TagDefinition& tag = database_.definition(i);
        const TagValue& value = database_.value(i);
        out << "{\"name\":\"" << json_escape(tag.name) << "\",\"description\":\""
            << json_escape(tag.description) << "\",\"unit\":\"" << json_escape(tag.unit)
            << "\",\"value\":" << (value.initialized ? json_number(value.value) : "null")
            << ",\"quality\":\"" << to_string(value.quality) << "\",\"timestamp\":"
            << value.timestamp_ms << ",\"alarm\":\"";

        AlarmState worst = AlarmState::Normal;
        for (std::size_t level = 0; level < kAlarmLevelCount; ++level) {
            const AlarmState state = alarms_.state(i, static_cast<AlarmLevel>(level));
            if (state == AlarmState::UnackedActive) {
                worst = state;
                break;
            }
            if (state == AlarmState::AckedActive) {
                worst = state;
            }
        }
        out << to_string(worst) << "\",\"limits\":[" << (std::isfinite(tag.limits.lolo) ? 1 : 0)
            << ',' << (std::isfinite(tag.limits.lo) ? 1 : 0) << ','
            << (std::isfinite(tag.limits.hi) ? 1 : 0) << ','
            << (std::isfinite(tag.limits.hihi) ? 1 : 0) << "]}";
    }
    out << "]}";
    return out.str();
}

std::string Gateway::alarms_json() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::ostringstream out;
    out << "{\"unacked\":" << alarms_.unacknowledged_count() << ",\"standing\":[";
    const std::vector<AlarmEvent> standing = alarms_.standing_alarms();
    for (std::size_t i = 0; i < standing.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << "{\"tag\":\"" << json_escape(standing[i].tag) << "\",\"level\":\""
            << to_string(standing[i].level) << "\",\"state\":\""
            << to_string(standing[i].state) << "\"}";
    }
    out << "],\"log\":[";
    const std::vector<AlarmEvent>& log = alarms_.log();
    const std::size_t first = log.size() > 20 ? log.size() - 20 : 0;
    for (std::size_t i = first; i < log.size(); ++i) {
        if (i != first) {
            out << ',';
        }
        out << "{\"time\":\"" << json_escape(format_timestamp(log[i].timestamp_ms))
            << "\",\"tag\":\"" << json_escape(log[i].tag) << "\",\"level\":\""
            << to_string(log[i].level) << "\",\"state\":\"" << to_string(log[i].state)
            << "\",\"value\":" << json_number(log[i].value)
            << ",\"limit\":" << json_number(log[i].limit)
            << ",\"return\":" << (log[i].is_return ? "true" : "false") << "}";
    }
    out << "]}";
    return out.str();
}

std::string Gateway::history_json(std::size_t tag_index) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::ostringstream out;
    out << '[';
    const std::deque<TrendPoint>& points = history_.series(tag_index);
    bool first = true;
    for (const TrendPoint& point : points) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << '[' << point.timestamp_ms << ',' << json_number(point.value) << ']';
    }
    out << ']';
    return out.str();
}

}  // namespace scada
