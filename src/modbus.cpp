#include "scada/modbus.hpp"

#include <stdexcept>

namespace scada::modbus {

const char* to_string(Exception exception) {
    switch (exception) {
        case Exception::None:
            return "none";
        case Exception::IllegalFunction:
            return "illegal function";
        case Exception::IllegalDataAddress:
            return "illegal data address";
        case Exception::IllegalDataValue:
            return "illegal data value";
        case Exception::ServerDeviceFailure:
            return "server device failure";
        case Exception::Acknowledge:
            return "acknowledge";
        case Exception::ServerDeviceBusy:
            return "server device busy";
        case Exception::MemoryParityError:
            return "memory parity error";
        case Exception::GatewayPathUnavailable:
            return "gateway path unavailable";
        case Exception::GatewayTargetFailedToRespond:
            return "gateway target failed to respond";
    }
    return "unknown";
}

std::uint16_t crc16(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x0001u) != 0u) {
                crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xA001u);
            } else {
                crc = static_cast<std::uint16_t>(crc >> 1);
            }
        }
    }
    return crc;
}

std::uint16_t crc16(const Bytes& data) { return crc16(data.data(), data.size()); }

namespace {

void put_u16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t get_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

}  // namespace

Bytes make_read_request(std::uint8_t function, std::uint16_t address, std::uint16_t count) {
    Bytes pdu;
    pdu.reserve(5);
    pdu.push_back(function);
    put_u16(pdu, address);
    put_u16(pdu, count);
    return pdu;
}

Bytes make_write_single_coil(std::uint16_t address, bool on) {
    Bytes pdu;
    pdu.reserve(5);
    pdu.push_back(kWriteSingleCoil);
    put_u16(pdu, address);
    put_u16(pdu, on ? 0xFF00 : 0x0000);
    return pdu;
}

Bytes make_write_single_register(std::uint16_t address, std::uint16_t value) {
    Bytes pdu;
    pdu.reserve(5);
    pdu.push_back(kWriteSingleRegister);
    put_u16(pdu, address);
    put_u16(pdu, value);
    return pdu;
}

bool is_exception_response(const std::uint8_t* pdu, std::size_t size) {
    return size == 2 && (pdu[0] & 0x80) != 0;
}

Exception exception_code(const std::uint8_t* pdu, std::size_t size) {
    if (size < 2) {
        return Exception::ServerDeviceFailure;
    }
    return static_cast<Exception>(pdu[1]);
}

bool parse_read_response(std::uint8_t function, const std::uint8_t* pdu, std::size_t size,
                         ReadResponse& out) {
    out = ReadResponse{};
    if (size < 2) {
        return false;
    }

    if ((pdu[0] & 0x80) != 0) {
        out.is_exception = true;
        out.exception = static_cast<Exception>(pdu[1]);
        return (pdu[0] & 0x7F) == function;
    }

    if (pdu[0] != function) {
        return false;
    }

    const std::size_t byte_count = pdu[1];
    if (size != 2 + byte_count) {
        return false;
    }

    const std::uint8_t* payload = pdu + 2;
    switch (function) {
        case kReadHoldingRegisters:
        case kReadInputRegisters: {
            if (byte_count % 2 != 0) {
                return false;
            }
            out.registers.reserve(byte_count / 2);
            for (std::size_t i = 0; i + 1 < byte_count; i += 2) {
                out.registers.push_back(get_u16(payload + i));
            }
            return true;
        }
        case kReadCoils:
        case kReadDiscreteInputs: {
            out.bits.reserve(byte_count * 8);
            for (std::size_t i = 0; i < byte_count; ++i) {
                for (int bit = 0; bit < 8; ++bit) {
                    out.bits.push_back(((payload[i] >> bit) & 0x01u) != 0u);
                }
            }
            return true;
        }
        default:
            return false;
    }
}

Bytes wrap_mbap(std::uint16_t transaction, std::uint8_t unit, const Bytes& pdu) {
    Bytes frame;
    frame.reserve(kMbapHeaderSize + pdu.size());
    put_u16(frame, transaction);
    put_u16(frame, kProtocolId);
    put_u16(frame, static_cast<std::uint16_t>(pdu.size() + 1));  // unit id + pdu
    frame.push_back(unit);
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    return frame;
}

std::size_t mbap_frame_length(const std::uint8_t* buffer, std::size_t available) {
    if (available < 6) {
        return 0;  // cannot read the length field yet
    }
    if (get_u16(buffer + 2) != kProtocolId) {
        return 0;
    }
    const std::size_t payload = get_u16(buffer + 4);
    // length counts the unit id plus the pdu, and the header is 6 bytes before it
    return 6 + payload;
}

bool unwrap_mbap(const std::uint8_t* frame, std::size_t size, MbapFrame& out) {
    if (size < kMbapHeaderSize) {
        return false;
    }
    if (get_u16(frame + 2) != kProtocolId) {
        return false;
    }
    const std::size_t payload = get_u16(frame + 4);
    if (6 + payload != size) {
        return false;
    }
    out.transaction = get_u16(frame);
    out.unit = frame[6];
    out.pdu.assign(frame + kMbapHeaderSize, frame + size);
    return true;
}

Bytes wrap_rtu(std::uint8_t unit, const Bytes& pdu) {
    Bytes frame;
    frame.reserve(pdu.size() + 3);
    frame.push_back(unit);
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    const std::uint16_t crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));         // low byte first
    frame.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
    return frame;
}

bool unwrap_rtu(const std::uint8_t* frame, std::size_t size, RtuFrame& out) {
    if (size < 4) {
        return false;
    }
    const std::uint16_t expected = static_cast<std::uint16_t>(
        frame[size - 2] | (static_cast<std::uint16_t>(frame[size - 1]) << 8));
    if (crc16(frame, size - 2) != expected) {
        return false;
    }
    out.unit = frame[0];
    out.pdu.assign(frame + 1, frame + size - 2);
    return true;
}

}  // namespace scada::modbus
