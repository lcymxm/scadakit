// Alarm engine following the shape of ISA-18.2: each limit on a tag is its own
// alarm with its own lifecycle (unacknowledged active -> acknowledged active ->
// cleared), so an operator can always tell a new alarm from an old one that is
// still standing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scada/tag.hpp"
#include "scada/types.hpp"

namespace scada {

enum class AlarmLevel { Hihi = 0, Hi = 1, Lo = 2, Lolo = 3 };
constexpr std::size_t kAlarmLevelCount = 4;

const char* to_string(AlarmLevel level);

// The four states of the ISA-18.2 state machine, minus the shelving and
// suppression features this project does not implement.
enum class AlarmState {
    Normal,           // condition gone, acknowledged
    UnackedActive,    // in alarm, nobody has seen it yet
    AckedActive,      // in alarm, acknowledged, still standing
    UnackedReturned,  // went away before anyone acknowledged it
};

const char* to_string(AlarmState state);

struct AlarmEvent {
    std::string tag;
    AlarmLevel level = AlarmLevel::Hi;
    AlarmState state = AlarmState::Normal;
    double value = 0.0;
    double limit = 0.0;
    std::int64_t timestamp_ms = 0;
    bool is_return = false;
};

class AlarmEngine {
public:
    explicit AlarmEngine(const TagTable& table);

    // Feeds one sample. Samples with unusable quality are ignored: a dead device
    // must not raise or clear alarms on the operator's screen.
    void evaluate(std::size_t tag_index, double value, Quality quality,
                  std::int64_t timestamp_ms);

    // Acknowledges every standing alarm of one tag. Returns how many changed.
    std::size_t acknowledge(const std::string& tag, std::int64_t timestamp_ms);
    std::size_t acknowledge_all(std::int64_t timestamp_ms);

    AlarmState state(std::size_t tag_index, AlarmLevel level) const;
    bool condition_active(std::size_t tag_index, AlarmLevel level) const;

    // Unacknowledged first, then by level (HH before HI before LO before LL).
    std::vector<AlarmEvent> standing_alarms() const;
    std::size_t unacknowledged_count() const;

    // Most severe level currently active, or -1 when nothing is in alarm.
    int highest_active_level() const;

    const std::vector<AlarmEvent>& log() const { return log_; }
    std::size_t capacity() const { return capacity_; }

private:
    struct Condition {
        bool active = false;
        bool acknowledged = true;
        bool defined = false;
        double limit = 0.0;
    };

    std::size_t slot(std::size_t tag_index, AlarmLevel level) const {
        return tag_index * kAlarmLevelCount + static_cast<std::size_t>(level);
    }
    void raise(Condition& condition, std::size_t tag_index, AlarmLevel level, double value,
               std::int64_t timestamp_ms);
    void clear(Condition& condition, std::size_t tag_index, AlarmLevel level, double value,
               std::int64_t timestamp_ms);
    void record(AlarmEvent event);

    std::vector<TagDefinition> definitions_;
    std::vector<Condition> conditions_;
    std::vector<AlarmEvent> log_;
    std::size_t capacity_ = 500;
};

}  // namespace scada
