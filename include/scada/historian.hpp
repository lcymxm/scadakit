// Trend buffer (趋势): a fixed number of samples per tag, oldest dropped first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "scada/tag.hpp"
#include "scada/types.hpp"

namespace scada {

struct TrendPoint {
    std::int64_t timestamp_ms = 0;
    double value = 0.0;
    Quality quality = Quality::Good;
};

class Historian {
public:
    explicit Historian(std::size_t tag_count, std::size_t capacity_per_tag = 900);

    void append(std::size_t index, std::int64_t timestamp_ms, double value, Quality quality);

    const std::deque<TrendPoint>& series(std::size_t index) const;
    std::size_t capacity() const { return capacity_; }
    std::size_t size() const { return series_.size(); }

    // Widest-to-narrowest CSV of every series, one row per sample.
    std::string to_csv(const TagTable& table) const;

private:
    std::size_t capacity_;
    std::vector<std::deque<TrendPoint>> series_;
    std::deque<TrendPoint> empty_;
};

}  // namespace scada
