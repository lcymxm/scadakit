#include "scada/master.hpp"

#include <cstring>

namespace scada {
namespace {

constexpr std::size_t kMaxRtuFrame = 256;

}  // namespace

ModbusMaster::ModbusMaster(MasterConfig config) : config_(std::move(config)) {}

bool ModbusMaster::ensure_connected(std::string& error) {
    if (socket_.valid()) {
        return true;
    }
    socket_ = TcpSocket::connect_to(config_.host, config_.port, config_.timeout_ms, error);
    return socket_.valid();
}

void ModbusMaster::disconnect() { socket_.close(); }

bool ModbusMaster::transact(const modbus::Bytes& request, modbus::Bytes& response,
                            std::string& error) {
    if (config_.rtu_over_tcp) {
        return transact_rtu(request, response, error);
    }

    const modbus::Bytes frame = modbus::wrap_mbap(transaction_, config_.station, request);
    if (socket_.send_bytes(frame.data(), frame.size()) < 0) {
        error = "send failed";
        return false;
    }

    // Read the header first, then exactly the number of bytes it announces.
    std::uint8_t header[modbus::kMbapHeaderSize];
    if (!socket_.receive_exactly(header, sizeof(header), config_.timeout_ms)) {
        error = "no response";
        return false;
    }

    modbus::MbapFrame mbap;
    // The parser wants the whole frame, so reconstruct the announced length and
    // read the remainder before validating.
    const std::size_t total = modbus::mbap_frame_length(header, sizeof(header));
    if (total < modbus::kMbapHeaderSize || total > kMaxRtuFrame) {
        error = "malformed MBAP header";
        return false;
    }

    modbus::Bytes buffer(total);
    std::memcpy(buffer.data(), header, sizeof(header));
    if (total > sizeof(header)) {
        if (!socket_.receive_exactly(buffer.data() + sizeof(header), total - sizeof(header),
                                     config_.timeout_ms)) {
            error = "truncated response";
            return false;
        }
    }

    if (!modbus::unwrap_mbap(buffer.data(), buffer.size(), mbap)) {
        error = "malformed MBAP frame";
        return false;
    }
    if (mbap.transaction != transaction_) {
        error = "transaction id mismatch";
        return false;
    }
    response = std::move(mbap.pdu);
    return true;
}

bool ModbusMaster::transact_rtu(const modbus::Bytes& request, modbus::Bytes& response,
                                std::string& error) {
    const modbus::Bytes frame = modbus::wrap_rtu(config_.station, request);
    if (socket_.send_bytes(frame.data(), frame.size()) < 0) {
        error = "send failed";
        return false;
    }

    // unit, function, then either a byte count or an exception code
    std::uint8_t head[3];
    if (!socket_.receive_exactly(head, sizeof(head), config_.timeout_ms)) {
        error = "no response";
        return false;
    }

    std::size_t total = 0;
    if ((head[1] & 0x80) != 0) {
        total = 5;  // unit + function + code + crc
    } else {
        total = 3 + static_cast<std::size_t>(head[2]) + 2;
    }
    if (total > kMaxRtuFrame) {
        error = "oversized RTU frame";
        return false;
    }

    modbus::Bytes buffer(total);
    std::memcpy(buffer.data(), head, sizeof(head));
    if (total > sizeof(head)) {
        if (!socket_.receive_exactly(buffer.data() + sizeof(head), total - sizeof(head),
                                     config_.timeout_ms)) {
            error = "truncated RTU frame";
            return false;
        }
    }

    modbus::RtuFrame rtu;
    if (!modbus::unwrap_rtu(buffer.data(), buffer.size(), rtu)) {
        error = "CRC check failed";
        return false;
    }
    if (rtu.unit != config_.station) {
        error = "unexpected station address";
        return false;
    }
    response = std::move(rtu.pdu);
    return true;
}

bool ModbusMaster::read(std::uint8_t function, std::uint16_t address, std::uint16_t count,
                        modbus::ReadResponse& response, std::string& error) {
    const modbus::Bytes request = modbus::make_read_request(function, address, count);

    for (int attempt = 0; attempt <= config_.retries; ++attempt) {
        if (!ensure_connected(error)) {
            ++consecutive_failures_;
            continue;
        }

        modbus::Bytes payload;
        if (!transact(request, payload, error)) {
            // A timeout usually means the connection is wedged; start over.
            disconnect();
            ++consecutive_failures_;
            continue;
        }

        modbus::ReadResponse parsed;
        if (!modbus::parse_read_response(function, payload.data(), payload.size(), parsed)) {
            error = "malformed response PDU";
            ++consecutive_failures_;
            continue;
        }

        response = std::move(parsed);
        consecutive_failures_ = 0;
        ++transaction_;
        return true;
    }
    return false;
}

bool ModbusMaster::write_single_coil(std::uint16_t address, bool on, std::string& error) {
    const modbus::Bytes request = modbus::make_write_single_coil(address, on);
    for (int attempt = 0; attempt <= config_.retries; ++attempt) {
        if (!ensure_connected(error)) {
            ++consecutive_failures_;
            continue;
        }
        modbus::Bytes payload;
        if (!transact(request, payload, error)) {
            disconnect();
            ++consecutive_failures_;
            continue;
        }
        // A write is acknowledged by echoing the request back.
        if (payload.size() != request.size() ||
            std::memcmp(payload.data(), request.data(), request.size()) != 0) {
            error = "write not acknowledged";
            ++consecutive_failures_;
            continue;
        }
        consecutive_failures_ = 0;
        ++transaction_;
        return true;
    }
    return false;
}

bool ModbusMaster::write_single_register(std::uint16_t address, std::uint16_t value,
                                         std::string& error) {
    const modbus::Bytes request = modbus::make_write_single_register(address, value);
    for (int attempt = 0; attempt <= config_.retries; ++attempt) {
        if (!ensure_connected(error)) {
            ++consecutive_failures_;
            continue;
        }
        modbus::Bytes payload;
        if (!transact(request, payload, error)) {
            disconnect();
            ++consecutive_failures_;
            continue;
        }
        if (payload.size() != request.size() ||
            std::memcmp(payload.data(), request.data(), request.size()) != 0) {
            error = "write not acknowledged";
            ++consecutive_failures_;
            continue;
        }
        consecutive_failures_ = 0;
        ++transaction_;
        return true;
    }
    return false;
}

}  // namespace scada
