// Modbus codec: CRC16, protocol data units, and the two framings a gateway has to
// speak -- MBAP for Modbus TCP and the RTU frame (with its CRC) for serial links
// and for the many devices that tunnel RTU over TCP.
//
// Everything here is pure byte manipulation, which is why it is the easiest part
// of the project to test exhaustively.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scada::modbus {

using Bytes = std::vector<std::uint8_t>;

// Function codes this gateway uses.
constexpr std::uint8_t kReadCoils = 0x01;
constexpr std::uint8_t kReadDiscreteInputs = 0x02;
constexpr std::uint8_t kReadHoldingRegisters = 0x03;
constexpr std::uint8_t kReadInputRegisters = 0x04;
constexpr std::uint8_t kWriteSingleCoil = 0x05;
constexpr std::uint8_t kWriteSingleRegister = 0x06;

// The specification's limits on one request.
constexpr std::uint16_t kMaxReadRegisters = 125;
constexpr std::uint16_t kMaxReadBits = 2000;

constexpr std::size_t kMbapHeaderSize = 7;  // transaction, protocol, length, unit
constexpr std::uint16_t kProtocolId = 0;

enum class Exception : std::uint8_t {
    None = 0x00,
    IllegalFunction = 0x01,
    IllegalDataAddress = 0x02,
    IllegalDataValue = 0x03,
    ServerDeviceFailure = 0x04,
    Acknowledge = 0x05,
    ServerDeviceBusy = 0x06,
    MemoryParityError = 0x08,
    GatewayPathUnavailable = 0x0A,
    GatewayTargetFailedToRespond = 0x0B,
};

const char* to_string(Exception exception);

// CRC-16/MODBUS: reflected polynomial 0xA001, seed 0xFFFF, no final XOR. On the
// wire the low byte goes first.
std::uint16_t crc16(const std::uint8_t* data, std::size_t size);
std::uint16_t crc16(const Bytes& data);

// ---- protocol data units -----------------------------------------------------

// [function, address_hi, address_lo, count_hi, count_lo]
Bytes make_read_request(std::uint8_t function, std::uint16_t address, std::uint16_t count);

// [function, address_hi, address_lo, value_hi, value_lo]
Bytes make_write_single_coil(std::uint16_t address, bool on);
Bytes make_write_single_register(std::uint16_t address, std::uint16_t value);

struct ReadResponse {
    bool is_exception = false;
    Exception exception = Exception::None;
    std::vector<std::uint16_t> registers;  // for the register functions
    std::vector<bool> bits;                // for the bit functions
};

// Parses the payload of a response PDU. Returns false when the bytes cannot be a
// valid response for `function` (wrong length, wrong function echo, ...).
bool parse_read_response(std::uint8_t function, const std::uint8_t* pdu, std::size_t size,
                         ReadResponse& out);

// Exception responses are [function | 0x80, code].
bool is_exception_response(const std::uint8_t* pdu, std::size_t size);
Exception exception_code(const std::uint8_t* pdu, std::size_t size);

// ---- MBAP (Modbus TCP) -------------------------------------------------------

Bytes wrap_mbap(std::uint16_t transaction, std::uint8_t unit, const Bytes& pdu);

struct MbapFrame {
    std::uint16_t transaction = 0;
    std::uint8_t unit = 0;
    Bytes pdu;
};

// `size` must be the whole frame including the header. Returns false when the
// header is inconsistent (wrong protocol id, length field that does not match).
bool unwrap_mbap(const std::uint8_t* frame, std::size_t size, MbapFrame& out);

// Length of the frame starting at `buffer`, or 0 when more bytes are needed.
// Used by the server to know when a request is complete.
std::size_t mbap_frame_length(const std::uint8_t* buffer, std::size_t available);

// ---- RTU framing -------------------------------------------------------------

Bytes wrap_rtu(std::uint8_t unit, const Bytes& pdu);

struct RtuFrame {
    std::uint8_t unit = 0;
    Bytes pdu;
};

bool unwrap_rtu(const std::uint8_t* frame, std::size_t size, RtuFrame& out);

}  // namespace scada::modbus
