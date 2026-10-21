// The real-time database (实时库): one current value per tag, with its quality and
// the time it was last refreshed. Everything the HMI shows comes from here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scada/tag.hpp"
#include "scada/types.hpp"

namespace scada {

struct TagValue {
    double value = 0.0;
    Quality quality = Quality::Bad;
    std::int64_t timestamp_ms = 0;
    bool initialized = false;
};

class RuntimeDatabase {
public:
    explicit RuntimeDatabase(const TagTable& table);

    std::size_t size() const { return values_.size(); }
    const TagDefinition& definition(std::size_t index) const { return definitions_[index]; }
    // Returns size() when the tag does not exist.
    std::size_t index_of(const std::string& name) const;
    const TagValue& value(std::size_t index) const { return values_[index]; }
    TagValue& value(std::size_t index) { return values_[index]; }

    // Stores a sample. The first sample of a tag is always kept; after that a
    // change smaller than the tag's deadband is dropped, which is what keeps the
    // historian and the JSON snapshot from flooding with noise.
    bool update(std::size_t index, double value, Quality quality, std::int64_t timestamp_ms);

    // Forces a quality change without a new value (device went offline, value
    // went stale ...).
    void set_quality(std::size_t index, Quality quality);

    std::size_t good_count() const;
    std::size_t bad_count() const;

private:
    std::vector<TagDefinition> definitions_;
    std::vector<TagValue> values_;
};

}  // namespace scada
