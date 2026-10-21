#include <cmath>
#include <cstring>
#include <string>

#include "scada/tag.hpp"
#include "test_framework.hpp"

using namespace scada;

namespace {

const char* kMinimal = R"(
# a comment
[tag:TI-101]
description = 主变油温
unit = °C
station = 1
register = input:0
type = int16
gain = 0.1
deadband = 0.2
hi = 58
hihi = 65
)";

}  // namespace

TEST(Tag, ParsesMinimalConfiguration) {
    const TagTable table = TagTable::parse(kMinimal);
    CHECK_EQ(table.size(), std::size_t{1});

    const TagDefinition& tag = table.tags()[0];
    CHECK_EQ(tag.name, std::string{"TI-101"});
    CHECK_EQ(tag.description, std::string{"主变油温"});
    CHECK_EQ(tag.unit, std::string{"°C"});
    CHECK_EQ(tag.station, std::uint8_t{1});
    CHECK(tag.address.kind == RegisterKind::InputRegister);
    CHECK(tag.address.type == DataType::Int16);
    CHECK_EQ(tag.address.offset, std::uint16_t{0});
    CHECK_NEAR(tag.gain, 0.1, 1e-12);
    CHECK_NEAR(tag.sample_deadband, 0.2, 1e-12);
    CHECK_NEAR(tag.limits.hi, 58.0, 1e-12);
    CHECK_NEAR(tag.limits.hihi, 65.0, 1e-12);
    CHECK(!std::isfinite(tag.limits.lo));
}

TEST(Tag, UnderstandsClassicFiveDigitReferences) {
    const TagTable table = TagTable::parse(R"(
[tag:A]
register = 30001
type = uint16
[tag:B]
register = 40011
type = uint16
[tag:C]
register = 00005
type = bit
[tag:D]
register = 10002
type = bit
)");
    CHECK(table.find("A")->address.kind == RegisterKind::InputRegister);
    CHECK_EQ(table.find("A")->address.offset, std::uint16_t{0});
    CHECK(table.find("B")->address.kind == RegisterKind::HoldingRegister);
    CHECK_EQ(table.find("B")->address.offset, std::uint16_t{10});
    CHECK(table.find("C")->address.kind == RegisterKind::Coil);
    CHECK_EQ(table.find("C")->address.offset, std::uint16_t{4});
    CHECK(table.find("D")->address.kind == RegisterKind::DiscreteInput);
    CHECK_EQ(table.find("D")->address.offset, std::uint16_t{1});
}

TEST(Tag, InfersBitForCoils) {
    // A coil without an explicit type is a single bit; anything else would be a
    // configuration error rather than a guess.
    const TagTable table = TagTable::parse(R"(
[tag:Q0]
register = coil:3
)");
    CHECK(table.find("Q0")->address.type == DataType::Bit);
    CHECK_EQ(table.find("Q0")->address.offset, std::uint16_t{3});
}

TEST(Tag, RejectsBitOnARegister) {
    CHECK_THROWS_AS(TagTable::parse(R"(
[tag:X]
register = holding:0
type = bit
)"),
                    "needs a coil or discrete input register");
}

TEST(Tag, RejectsNonBitOnACoil) {
    CHECK_THROWS_AS(TagTable::parse(R"(
[tag:X]
register = coil:0
type = uint16
)"),
                    "must have type 'bit'");
}

TEST(Tag, RejectsDuplicateNames) {
    CHECK_THROWS_AS(TagTable::parse(R"(
[tag:X]
register = input:0
type = uint16
[tag:X]
register = input:1
type = uint16
)"),
                    "duplicate tag name");
}

TEST(Tag, RejectsUnknownKey) {
    CHECK_THROWS_AS(TagTable::parse(R"(
[tag:X]
register = input:0
type = uint16
colour = red
)"),
                    "unknown key");
}

TEST(Tag, RejectsMissingRegister) {
    CHECK_THROWS_AS(TagTable::parse(R"(
[tag:X]
type = uint16
)"),
                    "has no 'register'");
}

TEST(Tag, RejectsBadNumber) {
    CHECK_THROWS_AS(TagTable::parse(R"(
[tag:X]
register = input:0
type = uint16
gain = 0.1x
)"),
                    "is not a number");
}

TEST(Tag, DecodesSignedAndUnsigned) {
    const std::uint16_t minus_one[1] = {0xFFFF};
    TagAddress signed_address;
    signed_address.type = DataType::Int16;
    CHECK_NEAR(decode_raw(signed_address, minus_one, 1), -1.0, 1e-12);

    TagAddress unsigned_address;
    unsigned_address.type = DataType::UInt16;
    CHECK_NEAR(decode_raw(unsigned_address, minus_one, 1), 65535.0, 1e-12);
}

TEST(Tag, Decodes32BitWithHighWordFirst) {
    const std::uint16_t registers[2] = {0x0001, 0x0002};
    TagAddress address;
    address.type = DataType::UInt32;
    CHECK_NEAR(decode_raw(address, registers, 2), 65538.0, 1e-12);

    address.type = DataType::Int32;
    const std::uint16_t negative[2] = {0xFFFF, 0xFFFE};
    CHECK_NEAR(decode_raw(address, negative, 2), -2.0, 1e-12);
}

TEST(Tag, DecodesFloat32) {
    // 1.5f is 0x3FC00000, split across two registers high word first.
    const std::uint16_t registers[2] = {0x3FC0, 0x0000};
    TagAddress address;
    address.type = DataType::Float32;
    CHECK_NEAR(decode_raw(address, registers, 2), 1.5, 1e-6);
}

TEST(Tag, TurnsShortInputIntoZeroNotGarbage) {
    const std::uint16_t registers[1] = {0x1234};
    TagAddress address;
    address.type = DataType::UInt32;  // needs two registers
    CHECK_NEAR(decode_raw(address, registers, 1), 0.0, 1e-12);
}

TEST(Tag, AppliesGainAndOffset) {
    TagDefinition tag;
    tag.gain = 0.1;
    tag.offset = -5.0;
    CHECK_NEAR(to_engineering(tag, 1234.0), 118.4, 1e-9);
}

TEST(Tag, RegisterCountsAndFunctionCodes) {
    CHECK_EQ(register_count(DataType::Int16), std::uint16_t{1});
    CHECK_EQ(register_count(DataType::UInt32), std::uint16_t{2});
    CHECK_EQ(register_count(DataType::Float32), std::uint16_t{2});
    CHECK_EQ(function_for_kind(RegisterKind::InputRegister), std::uint8_t{4});
    CHECK_EQ(function_for_kind(RegisterKind::HoldingRegister), std::uint8_t{3});
    CHECK_EQ(function_for_kind(RegisterKind::Coil), std::uint8_t{1});
    CHECK_EQ(function_for_kind(RegisterKind::DiscreteInput), std::uint8_t{2});
}
