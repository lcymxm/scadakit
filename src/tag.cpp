#include "scada/tag.hpp"

#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "scada/modbus.hpp"

namespace scada {
namespace {

std::string trim(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
        --last;
    }
    return text.substr(first, last - first);
}

std::string lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

double parse_double(const std::string& value, int line) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error("line " + std::to_string(line) + ": '" + value +
                                 "' is not a number");
    }
}

long parse_integer(const std::string& value, int line) {
    try {
        std::size_t consumed = 0;
        const long parsed = std::stol(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error("line " + std::to_string(line) + ": '" + value +
                                 "' is not an integer");
    }
}

RegisterKind kind_from_name(const std::string& name, int line) {
    const std::string key = lower(name);
    if (key == "coil" || key == "coils") {
        return RegisterKind::Coil;
    }
    if (key == "discrete" || key == "discreteinput" || key == "di") {
        return RegisterKind::DiscreteInput;
    }
    if (key == "holding" || key == "holdingregister" || key == "hr") {
        return RegisterKind::HoldingRegister;
    }
    if (key == "input" || key == "inputregister" || key == "ir") {
        return RegisterKind::InputRegister;
    }
    throw std::runtime_error("line " + std::to_string(line) + ": unknown register kind '" +
                             name + "'");
}

DataType type_from_name(const std::string& name, int line) {
    const std::string key = lower(name);
    if (key == "bit" || key == "bool") {
        return DataType::Bit;
    }
    if (key == "int16" || key == "i16" || key == "short") {
        return DataType::Int16;
    }
    if (key == "uint16" || key == "u16" || key == "word") {
        return DataType::UInt16;
    }
    if (key == "int32" || key == "i32" || key == "long") {
        return DataType::Int32;
    }
    if (key == "uint32" || key == "u32" || key == "dword") {
        return DataType::UInt32;
    }
    if (key == "float32" || key == "float" || key == "real") {
        return DataType::Float32;
    }
    throw std::runtime_error("line " + std::to_string(line) + ": unknown data type '" + name +
                             "'");
}

// Accepts either "kind:offset" or the classic five digit reference
// (0xxxx coil, 1xxxx discrete input, 3xxxx input register, 4xxxx holding).
TagAddress parse_address(const std::string& value, int line) {
    TagAddress address;
    const std::size_t colon = value.find(':');
    if (colon != std::string::npos) {
        address.kind = kind_from_name(trim(value.substr(0, colon)), line);
        const long offset = parse_integer(trim(value.substr(colon + 1)), line);
        if (offset < 0 || offset > 65535) {
            throw std::runtime_error("line " + std::to_string(line) + ": register offset " +
                                     std::to_string(offset) + " is out of range");
        }
        address.offset = static_cast<std::uint16_t>(offset);
        return address;
    }

    const long reference = parse_integer(value, line);
    if (reference < 1 || reference > 49999) {
        throw std::runtime_error("line " + std::to_string(line) + ": register reference " +
                                 std::to_string(reference) + " is out of range");
    }
    const long ten_thousands = reference / 10000;
    switch (ten_thousands) {
        case 0:
            address.kind = RegisterKind::Coil;
            address.offset = static_cast<std::uint16_t>(reference - 1);
            break;
        case 1:
            address.kind = RegisterKind::DiscreteInput;
            address.offset = static_cast<std::uint16_t>(reference - 10001);
            break;
        case 3:
            address.kind = RegisterKind::InputRegister;
            address.offset = static_cast<std::uint16_t>(reference - 30001);
            break;
        case 4:
            address.kind = RegisterKind::HoldingRegister;
            address.offset = static_cast<std::uint16_t>(reference - 40001);
            break;
        default:
            throw std::runtime_error("line " + std::to_string(line) + ": register reference " +
                                     std::to_string(reference) +
                                     " must start with 0, 1, 3 or 4");
    }
    return address;
}

}  // namespace

const char* to_string(RegisterKind kind) {
    switch (kind) {
        case RegisterKind::Coil:
            return "coil";
        case RegisterKind::DiscreteInput:
            return "discrete-input";
        case RegisterKind::HoldingRegister:
            return "holding-register";
        case RegisterKind::InputRegister:
            return "input-register";
    }
    return "?";
}

const char* to_string(DataType type) {
    switch (type) {
        case DataType::Bit:
            return "bit";
        case DataType::Int16:
            return "int16";
        case DataType::UInt16:
            return "uint16";
        case DataType::Int32:
            return "int32";
        case DataType::UInt32:
            return "uint32";
        case DataType::Float32:
            return "float32";
    }
    return "?";
}

std::uint16_t register_count(DataType type) {
    switch (type) {
        case DataType::Bit:
        case DataType::Int16:
        case DataType::UInt16:
            return 1;
        case DataType::Int32:
        case DataType::UInt32:
        case DataType::Float32:
            return 2;
    }
    return 1;
}

std::uint8_t function_for_kind(RegisterKind kind) {
    switch (kind) {
        case RegisterKind::Coil:
            return modbus::kReadCoils;
        case RegisterKind::DiscreteInput:
            return modbus::kReadDiscreteInputs;
        case RegisterKind::HoldingRegister:
            return modbus::kReadHoldingRegisters;
        case RegisterKind::InputRegister:
            return modbus::kReadInputRegisters;
    }
    return modbus::kReadInputRegisters;
}

bool AlarmLimits::any_defined() const {
    return std::isfinite(hihi) || std::isfinite(hi) || std::isfinite(lo) || std::isfinite(lolo);
}

double decode_raw(const TagAddress& address, const std::uint16_t* registers, std::size_t count) {
    const std::uint16_t needed = register_count(address.type);
    if (registers == nullptr || count < needed) {
        return 0.0;
    }
    switch (address.type) {
        case DataType::Bit:
            return registers[0] != 0 ? 1.0 : 0.0;
        case DataType::Int16:
            return static_cast<double>(static_cast<std::int16_t>(registers[0]));
        case DataType::UInt16:
            return static_cast<double>(registers[0]);
        case DataType::Int32:
        case DataType::UInt32: {
            const std::uint32_t raw = (static_cast<std::uint32_t>(registers[0]) << 16) |
                                      static_cast<std::uint32_t>(registers[1]);
            if (address.type == DataType::Int32) {
                return static_cast<double>(static_cast<std::int32_t>(raw));
            }
            return static_cast<double>(raw);
        }
        case DataType::Float32: {
            const std::uint32_t raw = (static_cast<std::uint32_t>(registers[0]) << 16) |
                                      static_cast<std::uint32_t>(registers[1]);
            float value = 0.0F;
            std::memcpy(&value, &raw, sizeof(value));
            return static_cast<double>(value);
        }
    }
    return 0.0;
}

double decode_bit(bool bit) { return bit ? 1.0 : 0.0; }

double to_engineering(const TagDefinition& tag, double raw) { return raw * tag.gain + tag.offset; }

void TagTable::add(TagDefinition tag) {
    for (const TagDefinition& existing : tags_) {
        if (existing.name == tag.name) {
            throw std::runtime_error("duplicate tag name '" + tag.name + "'");
        }
    }
    tags_.push_back(std::move(tag));
}

const TagDefinition* TagTable::find(const std::string& name) const {
    for (const TagDefinition& tag : tags_) {
        if (tag.name == name) {
            return &tag;
        }
    }
    return nullptr;
}

TagTable TagTable::parse(const std::string& text) {
    TagTable table;
    std::istringstream stream(text);
    std::string line;
    int line_number = 0;

    TagDefinition current;
    bool in_tag = false;
    bool have_type = false;
    bool have_register = false;

    const auto finish = [&]() {
        if (!in_tag) {
            return;
        }
        if (current.name.empty()) {
            throw std::runtime_error("a [tag:...] section has no name");
        }
        if (!have_register) {
            throw std::runtime_error("tag '" + current.name + "' has no 'register'");
        }
        if (!have_type) {
            // A bit source is the only case where the type can be inferred.
            if (current.address.kind == RegisterKind::Coil ||
                current.address.kind == RegisterKind::DiscreteInput) {
                current.address.type = DataType::Bit;
            }
        }
        if (current.address.kind == RegisterKind::Coil ||
            current.address.kind == RegisterKind::DiscreteInput) {
            if (current.address.type != DataType::Bit) {
                throw std::runtime_error("tag '" + current.name +
                                         "': a coil or discrete input must have type 'bit'");
            }
        } else if (current.address.type == DataType::Bit) {
            throw std::runtime_error("tag '" + current.name +
                                     "': type 'bit' needs a coil or discrete input register");
        }
        table.add(current);
        current = TagDefinition{};
        in_tag = false;
        have_type = false;
        have_register = false;
    };

    while (std::getline(stream, line)) {
        ++line_number;
        std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#' || stripped[0] == ';') {
            continue;
        }
        if (stripped.front() == '[') {
            finish();
            if (stripped.size() < 6 || lower(stripped.substr(0, 5)) != "[tag:") {
                throw std::runtime_error("line " + std::to_string(line_number) +
                                         ": expected a [tag:NAME] section");
            }
            const std::size_t close = stripped.find(']');
            if (close == std::string::npos) {
                throw std::runtime_error("line " + std::to_string(line_number) +
                                         ": missing ']' in section header");
            }
            current = TagDefinition{};
            current.name = trim(stripped.substr(5, close - 5));
            if (current.name.empty()) {
                throw std::runtime_error("line " + std::to_string(line_number) +
                                         ": empty tag name");
            }
            in_tag = true;
            continue;
        }

        const std::size_t equals = stripped.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("line " + std::to_string(line_number) +
                                     ": expected key = value");
        }
        if (!in_tag) {
            throw std::runtime_error("line " + std::to_string(line_number) +
                                     ": settings must appear under a [tag:NAME] section");
        }
        const std::string key = lower(trim(stripped.substr(0, equals)));
        const std::string value = trim(stripped.substr(equals + 1));

        if (key == "description") {
            current.description = value;
        } else if (key == "unit") {
            current.unit = value;
        } else if (key == "station") {
            const long station = parse_integer(value, line_number);
            if (station < 0 || station > 255) {
                throw std::runtime_error("line " + std::to_string(line_number) +
                                         ": station must be 0..255");
            }
            current.station = static_cast<std::uint8_t>(station);
        } else if (key == "register" || key == "address") {
            current.address = parse_address(value, line_number);
            have_register = true;
        } else if (key == "type") {
            current.address.type = type_from_name(value, line_number);
            have_type = true;
        } else if (key == "gain" || key == "scale") {
            current.gain = parse_double(value, line_number);
        } else if (key == "offset") {
            current.offset = parse_double(value, line_number);
        } else if (key == "deadband") {
            current.sample_deadband = parse_double(value, line_number);
            current.limits.deadband = current.sample_deadband;
        } else if (key == "alarm-deadband") {
            current.limits.deadband = parse_double(value, line_number);
        } else if (key == "hihi") {
            current.limits.hihi = parse_double(value, line_number);
        } else if (key == "hi") {
            current.limits.hi = parse_double(value, line_number);
        } else if (key == "lo") {
            current.limits.lo = parse_double(value, line_number);
        } else if (key == "lolo") {
            current.limits.lolo = parse_double(value, line_number);
        } else {
            throw std::runtime_error("line " + std::to_string(line_number) + ": unknown key '" +
                                     key + "'");
        }
    }
    finish();
    return table;
}

TagTable TagTable::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open tag configuration '" + path + "'");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str());
}

}  // namespace scada
