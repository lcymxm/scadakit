// The polling engine: it decides which register blocks to read, drives the
// Modbus master, feeds the real-time database, the alarm engine and the trend
// buffer, and turns the result into JSON for the HMI.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "scada/alarm.hpp"
#include "scada/historian.hpp"
#include "scada/master.hpp"
#include "scada/rtdb.hpp"
#include "scada/tag.hpp"

namespace scada {

struct GatewayConfig {
    MasterConfig master;
    int poll_interval_ms = 500;
    std::size_t history_capacity = 900;
    // A value older than this is reported as stale even if reads still succeed.
    std::int64_t stale_after_ms = 5000;
    // Trends are stored on change, and at least this often even when the value
    // does not move -- otherwise a steady signal leaves an empty chart.
    std::int64_t history_interval_ms = 2000;
};

// One Modbus read covers a contiguous run of registers.
struct PollBlock {
    std::uint8_t station = 1;
    RegisterKind kind = RegisterKind::InputRegister;
    std::uint16_t start = 0;
    std::uint16_t count = 0;
    std::vector<std::size_t> tags;  // indices into the tag table
};

// Groups tags into as few reads as possible. Neighbouring tags are merged even
// when a few registers sit unused between them (`max_gap`), because one read of
// six registers is much cheaper than two reads of three.
std::vector<PollBlock> plan_blocks(const std::vector<TagDefinition>& tags,
                                   std::uint16_t max_gap = 4);

class Gateway {
public:
    Gateway(TagTable table, GatewayConfig config);

    const TagTable& tags() const { return table_; }
    const RuntimeDatabase& database() const { return database_; }
    const AlarmEngine& alarms() const { return alarms_; }
    const Historian& history() const { return history_; }
    const std::vector<PollBlock>& blocks() const { return blocks_; }
    const GatewayConfig& config() const { return config_; }

    // Runs one complete scan: read every block, update the database, evaluate
    // alarms, append trends and age out stale values.
    void poll_once(std::int64_t now_ms);

    std::size_t poll_count() const { return poll_count_; }
    bool device_online() const { return device_online_; }
    const std::string& last_error() const { return last_error_; }

    // Returns how many alarms were acknowledged.
    std::size_t acknowledge(const std::string& tag);

    std::string snapshot_json() const;
    std::string alarms_json() const;
    std::string history_json(std::size_t tag_index) const;

private:
    void mark_station_failed(std::uint8_t station);
    void age_out(std::int64_t now_ms);

    TagTable table_;
    GatewayConfig config_;
    RuntimeDatabase database_;
    AlarmEngine alarms_;
    Historian history_;
    std::vector<PollBlock> blocks_;
    ModbusMaster master_;
    bool device_online_ = false;
    std::size_t poll_count_ = 0;
    std::string last_error_;
    std::vector<std::int64_t> last_history_ms_;
    // The HMI thread reads the database while the polling thread writes it, so
    // the scan is split into a network phase (no lock, may block for a timeout)
    // and a short update phase (locked).
    mutable std::mutex mutex_;
};

// JSON helpers shared with the dashboard.
std::string json_escape(const std::string& text);
std::string json_number(double value);

}  // namespace scada
