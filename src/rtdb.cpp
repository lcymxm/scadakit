#include "scada/rtdb.hpp"

#include <cmath>

namespace scada {

RuntimeDatabase::RuntimeDatabase(const TagTable& table)
    : definitions_(table.tags()), values_(table.size()) {}

std::size_t RuntimeDatabase::index_of(const std::string& name) const {
    for (std::size_t i = 0; i < definitions_.size(); ++i) {
        if (definitions_[i].name == name) {
            return i;
        }
    }
    return definitions_.size();
}

bool RuntimeDatabase::update(std::size_t index, double value, Quality quality,
                             std::int64_t timestamp_ms) {
    if (index >= values_.size()) {
        return false;
    }
    TagValue& current = values_[index];
    const bool first = !current.initialized;
    const bool quality_changed = current.quality != quality;
    const double delta = std::fabs(value - current.value);
    if (!first && !quality_changed && delta < definitions_[index].sample_deadband) {
        // Still refresh the timestamp so "is this device alive" stays truthful.
        current.timestamp_ms = timestamp_ms;
        return false;
    }
    current.value = value;
    current.quality = quality;
    current.timestamp_ms = timestamp_ms;
    current.initialized = true;
    return true;
}

void RuntimeDatabase::set_quality(std::size_t index, Quality quality) {
    if (index < values_.size()) {
        values_[index].quality = quality;
    }
}

std::size_t RuntimeDatabase::good_count() const {
    std::size_t count = 0;
    for (const TagValue& value : values_) {
        if (value.quality == Quality::Good) {
            ++count;
        }
    }
    return count;
}

std::size_t RuntimeDatabase::bad_count() const {
    std::size_t count = 0;
    for (const TagValue& value : values_) {
        if (!is_usable(value.quality)) {
            ++count;
        }
    }
    return count;
}

}  // namespace scada
