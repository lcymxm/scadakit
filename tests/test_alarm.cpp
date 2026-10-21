#include <string>

#include "scada/alarm.hpp"
#include "scada/tag.hpp"
#include "test_framework.hpp"

using namespace scada;

namespace {

const char* kConfig = R"(
[tag:TI-101]
register = input:0
type = int16
alarm-deadband = 2
hi = 60
hihi = 80
lo = 10
lolo = 0

[tag:PT-101]
register = input:1
type = int16
hi = 10.7
)";

}  // namespace

TEST(Alarm, TripsAboveTheHighLimit) {
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);
    const std::size_t tag = 0;

    engine.evaluate(tag, 55.0, Quality::Good, 1000);
    CHECK(engine.state(tag, AlarmLevel::Hi) == AlarmState::Normal);

    engine.evaluate(tag, 61.0, Quality::Good, 2000);
    CHECK(engine.state(tag, AlarmLevel::Hi) == AlarmState::UnackedActive);
    CHECK(engine.condition_active(tag, AlarmLevel::Hi));
    CHECK_EQ(engine.unacknowledged_count(), std::size_t{1});
    CHECK_EQ(engine.log().size(), std::size_t{1});
    CHECK(!engine.log().back().is_return);
    CHECK_EQ(engine.log().back().timestamp_ms, std::int64_t{2000});
}

TEST(Alarm, HighHighIsSeparateFromHigh) {
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);
    const std::size_t tag = 0;

    engine.evaluate(tag, 85.0, Quality::Good, 1000);
    CHECK(engine.condition_active(tag, AlarmLevel::Hihi));
    CHECK(engine.condition_active(tag, AlarmLevel::Hi));
    CHECK_EQ(engine.unacknowledged_count(), std::size_t{2});
    CHECK_EQ(engine.highest_active_level(), 0);  // HH is the most severe
}

TEST(Alarm, AcknowledgeKeepsItStanding) {
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);

    engine.evaluate(0, 65.0, Quality::Good, 1000);
    CHECK_EQ(engine.acknowledge("TI-101", 1200), std::size_t{1});
    CHECK(engine.state(0, AlarmLevel::Hi) == AlarmState::AckedActive);
    CHECK(engine.condition_active(0, AlarmLevel::Hi));
    CHECK_EQ(engine.unacknowledged_count(), std::size_t{0});

    // A second acknowledge has nothing left to do.
    CHECK_EQ(engine.acknowledge("TI-101", 1300), std::size_t{0});
}

TEST(Alarm, ReturnToNormalAfterAcknowledge) {
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);

    engine.evaluate(0, 65.0, Quality::Good, 1000);
    engine.acknowledge("TI-101", 1100);
    // The value has to come back below the limit *minus the deadband* to clear.
    engine.evaluate(0, 59.0, Quality::Good, 2000);
    CHECK(engine.state(0, AlarmLevel::Hi) == AlarmState::AckedActive);
    engine.evaluate(0, 57.0, Quality::Good, 3000);
    CHECK(engine.state(0, AlarmLevel::Hi) == AlarmState::Normal);
    CHECK(!engine.condition_active(0, AlarmLevel::Hi));
}

TEST(Alarm, ReturnWithoutAcknowledgeIsRemembered) {
    // The operator never saw it. ISA-18.2 keeps that visible as a returned but
    // unacknowledged alarm instead of silently clearing it.
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);

    engine.evaluate(0, 65.0, Quality::Good, 1000);
    engine.evaluate(0, 50.0, Quality::Good, 2000);
    CHECK(engine.state(0, AlarmLevel::Hi) == AlarmState::UnackedReturned);
    CHECK_EQ(engine.unacknowledged_count(), std::size_t{1});

    engine.acknowledge("TI-101", 3000);
    CHECK(engine.state(0, AlarmLevel::Hi) == AlarmState::Normal);
}

TEST(Alarm, DeadbandStopsChatter) {
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);

    engine.evaluate(0, 61.0, Quality::Good, 1000);
    CHECK(engine.condition_active(0, AlarmLevel::Hi));

    // 59 is below the limit but inside the 2 degree deadband, so the alarm stays.
    engine.evaluate(0, 59.0, Quality::Good, 2000);
    CHECK(engine.condition_active(0, AlarmLevel::Hi));

    // 57 is outside it, so it clears.
    engine.evaluate(0, 57.0, Quality::Good, 3000);
    CHECK(!engine.condition_active(0, AlarmLevel::Hi));
}

TEST(Alarm, LowLimitsTripDownwards) {
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);

    engine.evaluate(0, -5.0, Quality::Good, 1000);
    CHECK(engine.condition_active(0, AlarmLevel::Lolo));
    CHECK(engine.condition_active(0, AlarmLevel::Lo));
    CHECK_EQ(engine.highest_active_level(), 3);  // LL
}

TEST(Alarm, IgnoresSamplesWithBadQuality) {
    // A dead device must not raise an alarm, and must not clear one either.
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);

    engine.evaluate(0, 65.0, Quality::Good, 1000);
    CHECK(engine.condition_active(0, AlarmLevel::Hi));

    engine.evaluate(0, 0.0, Quality::CommFailure, 2000);
    CHECK(engine.condition_active(0, AlarmLevel::Hi));

    engine.evaluate(0, 100.0, Quality::Stale, 3000);
    CHECK(engine.condition_active(0, AlarmLevel::Hi));
    CHECK(!engine.condition_active(0, AlarmLevel::Hihi));
}

TEST(Alarm, StandingListPutsUnacknowledgedFirst) {
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);

    engine.evaluate(1, 11.0, Quality::Good, 1000);  // PT-101 high
    engine.acknowledge("PT-101", 1100);
    engine.evaluate(0, 85.0, Quality::Good, 2000);  // TI-101 high-high and high

    const std::vector<AlarmEvent> standing = engine.standing_alarms();
    CHECK_EQ(standing.size(), std::size_t{3});
    CHECK(standing[0].state == AlarmState::UnackedActive);
    CHECK_EQ(standing[0].tag, std::string{"TI-101"});
    CHECK(standing[0].level == AlarmLevel::Hihi);
    CHECK(standing.back().state == AlarmState::AckedActive);
}

TEST(Alarm, AcknowledgeAllClearsTheCounter) {
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);

    engine.evaluate(0, 85.0, Quality::Good, 1000);
    engine.evaluate(1, 11.0, Quality::Good, 1000);
    CHECK_EQ(engine.unacknowledged_count(), std::size_t{3});

    CHECK_EQ(engine.acknowledge_all(2000), std::size_t{3});
    CHECK_EQ(engine.unacknowledged_count(), std::size_t{0});
    CHECK(engine.condition_active(0, AlarmLevel::Hihi));  // still standing, just seen
}

TEST(Alarm, UnknownTagAcknowledgeIsHarmless) {
    const TagTable table = TagTable::parse(kConfig);
    AlarmEngine engine(table);
    CHECK_EQ(engine.acknowledge("NOPE", 1000), std::size_t{0});
}
