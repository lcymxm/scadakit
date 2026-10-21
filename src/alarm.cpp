#include "scada/alarm.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace scada {
namespace {

double limit_of(const TagDefinition& tag, AlarmLevel level, bool& defined) {
    defined = true;
    switch (level) {
        case AlarmLevel::Hihi:
            defined = std::isfinite(tag.limits.hihi);
            return tag.limits.hihi;
        case AlarmLevel::Hi:
            defined = std::isfinite(tag.limits.hi);
            return tag.limits.hi;
        case AlarmLevel::Lo:
            defined = std::isfinite(tag.limits.lo);
            return tag.limits.lo;
        case AlarmLevel::Lolo:
            defined = std::isfinite(tag.limits.lolo);
            return tag.limits.lolo;
    }
    defined = false;
    return 0.0;
}

bool is_high(AlarmLevel level) { return level == AlarmLevel::Hihi || level == AlarmLevel::Hi; }

}  // namespace

const char* to_string(AlarmLevel level) {
    switch (level) {
        case AlarmLevel::Hihi:
            return "HH";
        case AlarmLevel::Hi:
            return "HI";
        case AlarmLevel::Lo:
            return "LO";
        case AlarmLevel::Lolo:
            return "LL";
    }
    return "?";
}

const char* to_string(AlarmState state) {
    switch (state) {
        case AlarmState::Normal:
            return "normal";
        case AlarmState::UnackedActive:
            return "unacked-active";
        case AlarmState::AckedActive:
            return "acked-active";
        case AlarmState::UnackedReturned:
            return "unacked-returned";
    }
    return "?";
}

AlarmEngine::AlarmEngine(const TagTable& table) : definitions_(table.tags()) {
    conditions_.assign(definitions_.size() * kAlarmLevelCount, Condition{});
    for (std::size_t i = 0; i < definitions_.size(); ++i) {
        for (std::size_t level = 0; level < kAlarmLevelCount; ++level) {
            bool defined = false;
            const double limit =
                limit_of(definitions_[i], static_cast<AlarmLevel>(level), defined);
            Condition& condition = conditions_[slot(i, static_cast<AlarmLevel>(level))];
            condition.defined = defined;
            condition.limit = limit;
        }
    }
}

void AlarmEngine::record(AlarmEvent event) {
    log_.push_back(std::move(event));
    if (log_.size() > capacity_) {
        log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(log_.size() - capacity_));
    }
}

void AlarmEngine::raise(Condition& condition, std::size_t tag_index, AlarmLevel level,
                        double value, std::int64_t timestamp_ms) {
    condition.active = true;
    condition.acknowledged = false;

    AlarmEvent event;
    event.tag = definitions_[tag_index].name;
    event.level = level;
    event.state = AlarmState::UnackedActive;
    event.value = value;
    event.limit = condition.limit;
    event.timestamp_ms = timestamp_ms;
    event.is_return = false;
    record(std::move(event));
}

void AlarmEngine::clear(Condition& condition, std::size_t tag_index, AlarmLevel level,
                        double value, std::int64_t timestamp_ms) {
    condition.active = false;

    AlarmEvent event;
    event.tag = definitions_[tag_index].name;
    event.level = level;
    // If nobody acknowledged the alarm before it went away the state becomes
    // "returned but unacknowledged" rather than silently normal.
    event.state = condition.acknowledged ? AlarmState::Normal : AlarmState::UnackedReturned;
    event.value = value;
    event.limit = condition.limit;
    event.timestamp_ms = timestamp_ms;
    event.is_return = true;
    record(std::move(event));
}

void AlarmEngine::evaluate(std::size_t tag_index, double value, Quality quality,
                           std::int64_t timestamp_ms) {
    if (tag_index >= definitions_.size() || !is_usable(quality)) {
        return;
    }
    const TagDefinition& tag = definitions_[tag_index];
    const double deadband = tag.limits.deadband;

    for (std::size_t level_index = 0; level_index < kAlarmLevelCount; ++level_index) {
        const AlarmLevel level = static_cast<AlarmLevel>(level_index);
        Condition& condition = conditions_[slot(tag_index, level)];
        if (!condition.defined) {
            continue;
        }

        // The limit is crossed with no hysteresis; it only clears once the value
        // has come back past the deadband. Otherwise a value sitting exactly on
        // the limit would toggle every scan.
        const bool exceeded = is_high(level) ? value > condition.limit : value < condition.limit;
        const bool still_out = is_high(level) ? value >= condition.limit - deadband
                                              : value <= condition.limit + deadband;

        if (!condition.active && exceeded) {
            raise(condition, tag_index, level, value, timestamp_ms);
        } else if (condition.active && !still_out) {
            clear(condition, tag_index, level, value, timestamp_ms);
        }
    }
}

std::size_t AlarmEngine::acknowledge(const std::string& tag_name, std::int64_t timestamp_ms) {
    std::size_t changed = 0;
    for (std::size_t i = 0; i < definitions_.size(); ++i) {
        if (definitions_[i].name != tag_name) {
            continue;
        }
        for (std::size_t level_index = 0; level_index < kAlarmLevelCount; ++level_index) {
            Condition& condition = conditions_[slot(i, static_cast<AlarmLevel>(level_index))];
            if (!condition.defined || condition.acknowledged) {
                continue;
            }
            condition.acknowledged = true;
            ++changed;

            AlarmEvent event;
            event.tag = definitions_[i].name;
            event.level = static_cast<AlarmLevel>(level_index);
            event.state = condition.active ? AlarmState::AckedActive : AlarmState::Normal;
            event.limit = condition.limit;
            event.timestamp_ms = timestamp_ms;
            event.is_return = !condition.active;
            record(std::move(event));
        }
    }
    return changed;
}

std::size_t AlarmEngine::acknowledge_all(std::int64_t timestamp_ms) {
    std::size_t changed = 0;
    for (const TagDefinition& tag : definitions_) {
        changed += acknowledge(tag.name, timestamp_ms);
    }
    return changed;
}

AlarmState AlarmEngine::state(std::size_t tag_index, AlarmLevel level) const {
    if (tag_index >= definitions_.size()) {
        return AlarmState::Normal;
    }
    const Condition& condition = conditions_[slot(tag_index, level)];
    if (condition.active) {
        return condition.acknowledged ? AlarmState::AckedActive : AlarmState::UnackedActive;
    }
    return condition.acknowledged ? AlarmState::Normal : AlarmState::UnackedReturned;
}

bool AlarmEngine::condition_active(std::size_t tag_index, AlarmLevel level) const {
    if (tag_index >= definitions_.size()) {
        return false;
    }
    return conditions_[slot(tag_index, level)].active;
}

std::vector<AlarmEvent> AlarmEngine::standing_alarms() const {
    std::vector<AlarmEvent> standing;
    for (std::size_t i = 0; i < definitions_.size(); ++i) {
        for (std::size_t level_index = 0; level_index < kAlarmLevelCount; ++level_index) {
            const AlarmLevel level = static_cast<AlarmLevel>(level_index);
            const Condition& condition = conditions_[slot(i, level)];
            if (!condition.active && condition.acknowledged) {
                continue;
            }
            AlarmEvent event;
            event.tag = definitions_[i].name;
            event.level = level;
            event.state = state(i, level);
            event.limit = condition.limit;
            event.timestamp_ms = 0;
            event.value = 0.0;
            standing.push_back(std::move(event));
        }
    }
    // Unacknowledged first, then the more severe levels.
    std::stable_sort(standing.begin(), standing.end(),
                     [](const AlarmEvent& a, const AlarmEvent& b) {
                         const bool a_unacked = a.state == AlarmState::UnackedActive ||
                                                a.state == AlarmState::UnackedReturned;
                         const bool b_unacked = b.state == AlarmState::UnackedActive ||
                                                b.state == AlarmState::UnackedReturned;
                         if (a_unacked != b_unacked) {
                             return a_unacked;
                         }
                         return static_cast<int>(a.level) < static_cast<int>(b.level);
                     });
    return standing;
}

std::size_t AlarmEngine::unacknowledged_count() const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < definitions_.size(); ++i) {
        for (std::size_t level_index = 0; level_index < kAlarmLevelCount; ++level_index) {
            const Condition& condition = conditions_[slot(i, static_cast<AlarmLevel>(level_index))];
            if (condition.defined && !condition.acknowledged) {
                ++count;
            }
        }
    }
    return count;
}

int AlarmEngine::highest_active_level() const {
    // Severity order, not declaration order: HH outranks LL, and both outrank the
    // ordinary HI and LO tiers. Returning the first active level in enum order
    // would report a low alarm while a low-low alarm was standing.
    static constexpr AlarmLevel kSeverity[] = {AlarmLevel::Hihi, AlarmLevel::Lolo, AlarmLevel::Hi,
                                               AlarmLevel::Lo};
    for (const AlarmLevel level : kSeverity) {
        for (std::size_t i = 0; i < definitions_.size(); ++i) {
            if (conditions_[slot(i, level)].active) {
                return static_cast<int>(level);
            }
        }
    }
    return -1;
}

}  // namespace scada
