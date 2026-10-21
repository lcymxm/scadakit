// The tag (点表) layer: a tag maps a named engineering quantity onto Modbus
// registers, scales the raw integers into engineering units, and carries the
// limits the alarm engine watches.
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace scada {

enum class RegisterKind { Coil, DiscreteInput, HoldingRegister, InputRegister };
enum class DataType { Bit, Int16, UInt16, Int32, UInt32, Float32 };

const char* to_string(RegisterKind kind);
const char* to_string(DataType type);

// Number of 16-bit registers a value occupies.
std::uint16_t register_count(DataType type);

// Modbus function code for reading a whole block of this kind.
std::uint8_t function_for_kind(RegisterKind kind);

struct TagAddress {
    RegisterKind kind = RegisterKind::InputRegister;
    DataType type = DataType::UInt16;
    std::uint16_t offset = 0;
};

struct AlarmLimits {
    double hihi = std::numeric_limits<double>::infinity();
    double hi = std::numeric_limits<double>::infinity();
    double lo = -std::numeric_limits<double>::infinity();
    double lolo = -std::numeric_limits<double>::infinity();
    // Hysteresis applied when a limit clears, so a value sitting on the limit
    // does not chatter.
    double deadband = 0.0;

    bool any_defined() const;
};

struct TagDefinition {
    std::string name;
    std::string description;
    std::string unit;
    std::uint8_t station = 1;
    TagAddress address;
    double gain = 1.0;
    double offset = 0.0;
    // Change smaller than this (in engineering units) is not worth a new sample.
    double sample_deadband = 0.0;
    AlarmLimits limits;
};

// Turns raw registers into a value in engineering units (before gain/offset).
double decode_raw(const TagAddress& address, const std::uint16_t* registers, std::size_t count);

// Same, for the bit kinds.
double decode_bit(bool bit);

// Full conversion: (raw * gain) + offset.
double to_engineering(const TagDefinition& tag, double raw);

class TagTable {
public:
    void add(TagDefinition tag);
    const std::vector<TagDefinition>& tags() const { return tags_; }
    std::size_t size() const { return tags_.size(); }
    const TagDefinition* find(const std::string& name) const;

    // Parses the configuration format documented in README.md. Throws
    // std::runtime_error with the offending line on any problem.
    static TagTable parse(const std::string& text);
    static TagTable load(const std::string& path);

private:
    std::vector<TagDefinition> tags_;
};

}  // namespace scada
