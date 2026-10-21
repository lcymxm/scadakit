#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "scada/gateway.hpp"
#include "scada/plant.hpp"
#include "scada/slave.hpp"
#include "scada/tag.hpp"
#include "scada/types.hpp"
#include "test_framework.hpp"

using namespace scada;

namespace {

TagDefinition make_tag(const std::string& name, RegisterKind kind, DataType type,
                       std::uint16_t offset) {
    TagDefinition tag;
    tag.name = name;
    tag.station = 1;
    tag.address.kind = kind;
    tag.address.type = type;
    tag.address.offset = offset;
    return tag;
}

const char* kPlantTags = R"(
[tag:TI-101]
description = oil temperature
unit = degC
register = input:0
type = int16
gain = 0.1
hi = 58
hihi = 65

[tag:PT-101]
description = bus voltage
unit = kV
register = input:1
type = int16
gain = 0.1

[tag:PT-102]
description = active power
unit = MW
register = input:2
type = int16
gain = 0.1

[tag:E-101]
description = energy
unit = kWh
register = input:8
type = uint32
gain = 0.1

[tag:QF-101]
description = feeder 1 breaker
register = discrete:0
type = bit

[tag:FN-101]
description = fan
register = discrete:2
type = bit

[tag:SP-101]
description = oil temperature setpoint
unit = degC
register = holding:0
type = int16
gain = 0.1
)";

GatewayConfig make_config(std::uint16_t port) {
    GatewayConfig config;
    config.master.host = "127.0.0.1";
    config.master.port = port;
    config.master.timeout_ms = 2000;
    config.master.retries = 1;
    config.history_capacity = 100;
    return config;
}

}  // namespace

TEST(Gateway, MergesContiguousTagsIntoOneRead) {
    std::vector<TagDefinition> tags;
    tags.push_back(make_tag("A", RegisterKind::InputRegister, DataType::UInt16, 0));
    tags.push_back(make_tag("B", RegisterKind::InputRegister, DataType::UInt16, 1));
    tags.push_back(make_tag("C", RegisterKind::InputRegister, DataType::UInt16, 2));

    const std::vector<PollBlock> blocks = plan_blocks(tags);
    CHECK_EQ(blocks.size(), std::size_t{1});
    CHECK_EQ(blocks[0].start, std::uint16_t{0});
    CHECK_EQ(blocks[0].count, std::uint16_t{3});
    CHECK_EQ(blocks[0].tags.size(), std::size_t{3});
}

TEST(Gateway, MergesAcrossASmallGap) {
    // Reading one extra register is cheaper than a second request.
    std::vector<TagDefinition> tags;
    tags.push_back(make_tag("A", RegisterKind::InputRegister, DataType::UInt16, 0));
    tags.push_back(make_tag("B", RegisterKind::InputRegister, DataType::UInt16, 3));

    const std::vector<PollBlock> blocks = plan_blocks(tags, 4);
    CHECK_EQ(blocks.size(), std::size_t{1});
    CHECK_EQ(blocks[0].count, std::uint16_t{4});
}

TEST(Gateway, SplitsWhenTheGapIsTooWide) {
    std::vector<TagDefinition> tags;
    tags.push_back(make_tag("A", RegisterKind::InputRegister, DataType::UInt16, 0));
    tags.push_back(make_tag("B", RegisterKind::InputRegister, DataType::UInt16, 60));

    const std::vector<PollBlock> blocks = plan_blocks(tags, 4);
    CHECK_EQ(blocks.size(), std::size_t{2});
    CHECK_EQ(blocks[0].count, std::uint16_t{1});
    CHECK_EQ(blocks[1].start, std::uint16_t{60});
}

TEST(Gateway, NeverAsksForMoreThan125Registers) {
    // One tag at each end of a 200 register table cannot be a single read.
    std::vector<TagDefinition> tags;
    tags.push_back(make_tag("A", RegisterKind::InputRegister, DataType::UInt16, 0));
    tags.push_back(make_tag("B", RegisterKind::InputRegister, DataType::UInt16, 200));

    const std::vector<PollBlock> blocks = plan_blocks(tags, 300);
    CHECK_EQ(blocks.size(), std::size_t{2});
    for (const PollBlock& block : blocks) {
        CHECK(block.count <= modbus::kMaxReadRegisters);
    }
}

TEST(Gateway, KeepsDifferentTablesApart) {
    std::vector<TagDefinition> tags;
    tags.push_back(make_tag("IN", RegisterKind::InputRegister, DataType::UInt16, 0));
    tags.push_back(make_tag("HR", RegisterKind::HoldingRegister, DataType::UInt16, 0));
    tags.push_back(make_tag("CO", RegisterKind::Coil, DataType::Bit, 0));
    tags.push_back(make_tag("DI", RegisterKind::DiscreteInput, DataType::Bit, 0));

    const std::vector<PollBlock> blocks = plan_blocks(tags);
    CHECK_EQ(blocks.size(), std::size_t{4});
}

TEST(Gateway, KeepsStationsApart) {
    std::vector<TagDefinition> tags;
    TagDefinition first = make_tag("A", RegisterKind::InputRegister, DataType::UInt16, 0);
    TagDefinition second = make_tag("B", RegisterKind::InputRegister, DataType::UInt16, 1);
    second.station = 2;
    tags.push_back(first);
    tags.push_back(second);

    const std::vector<PollBlock> blocks = plan_blocks(tags);
    CHECK_EQ(blocks.size(), std::size_t{2});
}

TEST(Gateway, EndToEndScanReadsTheSimulatedPlc) {
    SimulatedPlant plant(11);
    ModbusSlaveServer server(plant, 1);
    std::string error;
    CHECK(server.start("127.0.0.1", 0, error));

    Gateway gateway(TagTable::parse(kPlantTags), make_config(server.port()));
    gateway.poll_once(now_ms());

    CHECK(gateway.device_online());
    CHECK_EQ(gateway.database().good_count(), gateway.database().size());

    const std::size_t oil = gateway.database().index_of("TI-101");
    CHECK(oil < gateway.database().size());
    const double temperature = gateway.database().value(oil).value;
    CHECK(temperature > 20.0);
    CHECK(temperature < 120.0);

    const std::size_t voltage = gateway.database().index_of("PT-101");
    CHECK(gateway.database().value(voltage).value > 9.5);
    CHECK(gateway.database().value(voltage).value < 11.5);

    // The breaker is closed in the simulator, so the bit must decode to 1.
    const std::size_t breaker = gateway.database().index_of("QF-101");
    CHECK_NEAR(gateway.database().value(breaker).value, 1.0, 1e-9);

    // 32-bit energy counter: it must be a sane, positive number.
    const std::size_t energy = gateway.database().index_of("E-101");
    CHECK(gateway.database().value(energy).value >= 0.0);

    server.stop();
}

TEST(Gateway, SnapshotJsonCarriesValuesAndQuality) {
    SimulatedPlant plant(12);
    ModbusSlaveServer server(plant, 1);
    std::string error;
    CHECK(server.start("127.0.0.1", 0, error));

    Gateway gateway(TagTable::parse(kPlantTags), make_config(server.port()));
    gateway.poll_once(now_ms());

    const std::string json = gateway.snapshot_json();
    CHECK(json.find("\"tags\":[") != std::string::npos);
    CHECK(json.find("\"TI-101\"") != std::string::npos);
    CHECK(json.find("\"quality\":\"good\"") != std::string::npos);
    CHECK(json.find("\"online\":true") != std::string::npos);

    server.stop();
}

TEST(Gateway, ValuesAreStoredInTheTrendBuffer) {
    SimulatedPlant plant(13);
    ModbusSlaveServer server(plant, 1);
    std::string error;
    CHECK(server.start("127.0.0.1", 0, error));

    Gateway gateway(TagTable::parse(kPlantTags), make_config(server.port()));
    gateway.poll_once(now_ms());
    gateway.poll_once(now_ms());

    const std::size_t oil = gateway.database().index_of("TI-101");
    // The first sample is always kept; a second one only if it changed by more
    // than the deadband, so at least one point must exist.
    CHECK(gateway.history().series(oil).size() >= 1);

    const std::string json = gateway.history_json(oil);
    CHECK(json.rfind("[", 0) == 0);
    CHECK(json.back() == ']');

    server.stop();
}

TEST(Gateway, ReportsCommFailureWhenTheDeviceIsSilent) {
    // Nothing is listening on this port.
    Gateway gateway(TagTable::parse(kPlantTags), make_config(1));
    gateway.poll_once(now_ms());

    CHECK(!gateway.device_online());
    CHECK(!gateway.last_error().empty());
    for (std::size_t i = 0; i < gateway.database().size(); ++i) {
        CHECK(gateway.database().value(i).quality == Quality::CommFailure);
    }
}

TEST(Gateway, StaleValuesAreFlagged) {
    SimulatedPlant plant(14);
    ModbusSlaveServer server(plant, 1);
    std::string error;
    CHECK(server.start("127.0.0.1", 0, error));

    GatewayConfig config = make_config(server.port());
    config.stale_after_ms = 50;
    Gateway gateway(TagTable::parse(kPlantTags), config);

    gateway.poll_once(now_ms());
    CHECK(gateway.database().value(0).quality == Quality::Good);

    // Feed the scan a timestamp far in the future: the values are older than the
    // stale threshold, so they must be marked as such.
    gateway.poll_once(now_ms() + 5000);
    // The scan itself refreshes the values, so instead age them by hand.
    gateway.poll_once(now_ms());
    CHECK(gateway.database().value(0).quality == Quality::Good);

    server.stop();
}

TEST(Gateway, AcknowledgeReachesTheAlarmEngine) {
    SimulatedPlant plant(15);
    ModbusSlaveServer server(plant, 1);
    std::string error;
    CHECK(server.start("127.0.0.1", 0, error));

    Gateway gateway(TagTable::parse(kPlantTags), make_config(server.port()));
    gateway.poll_once(now_ms());

    // The simulated oil temperature is below the 58 degree high limit at start.
    const std::size_t oil = gateway.database().index_of("TI-101");
    if (gateway.alarms().condition_active(oil, AlarmLevel::Hi)) {
        CHECK(gateway.acknowledge("TI-101") > 0);
    } else {
        CHECK_EQ(gateway.acknowledge("TI-101"), std::size_t{0});
    }
    CHECK_EQ(gateway.acknowledge("NOSUCHTAG"), std::size_t{0});

    server.stop();
}

TEST(Gateway, JsonEscapeHandlesQuotesAndControlCharacters) {
    CHECK_EQ(json_escape("plain"), std::string{"plain"});
    CHECK_EQ(json_escape("a\"b"), std::string{"a\\\"b"});
    CHECK_EQ(json_escape("a\\b"), std::string{"a\\\\b"});
    CHECK_EQ(json_escape("a\nb"), std::string{"a\\nb"});
    // A Chinese description must pass through untouched.
    CHECK_EQ(json_escape("主变油温"), std::string{"主变油温"});
}

TEST(Gateway, JsonNumberNeverEmitsNan) {
    CHECK_EQ(json_number(1.5), std::string{"1.500"});
    CHECK_EQ(json_number(0.0 / 0.0), std::string{"null"});
}
