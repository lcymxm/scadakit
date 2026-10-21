#include "scada/historian.hpp"

#include <iomanip>
#include <sstream>

namespace scada {

Historian::Historian(std::size_t tag_count, std::size_t capacity_per_tag)
    : capacity_(capacity_per_tag), series_(tag_count) {}

void Historian::append(std::size_t index, std::int64_t timestamp_ms, double value,
                       Quality quality) {
    if (index >= series_.size() || capacity_ == 0) {
        return;
    }
    std::deque<TrendPoint>& points = series_[index];
    points.push_back(TrendPoint{timestamp_ms, value, quality});
    while (points.size() > capacity_) {
        points.pop_front();
    }
}

const std::deque<TrendPoint>& Historian::series(std::size_t index) const {
    if (index >= series_.size()) {
        return empty_;
    }
    return series_[index];
}

std::string Historian::to_csv(const TagTable& table) const {
    std::ostringstream out;
    out << "timestamp,tag,value,unit,quality\n";
    out << std::fixed << std::setprecision(3);
    for (std::size_t i = 0; i < series_.size() && i < table.tags().size(); ++i) {
        const TagDefinition& tag = table.tags()[i];
        for (const TrendPoint& point : series_[i]) {
            out << format_timestamp(point.timestamp_ms) << ',' << tag.name << ',' << point.value
                << ',' << tag.unit << ',' << to_string(point.quality) << '\n';
        }
    }
    return out.str();
}

}  // namespace scada
