// Modbus client ("master" in Modbus wording). It owns one TCP connection and
// hides the framing, the transaction ids and the retry policy from the polling
// engine.
#pragma once

#include <cstdint>
#include <string>

#include "scada/modbus.hpp"
#include "scada/socket.hpp"

namespace scada {

struct MasterConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 1502;
    std::uint8_t station = 1;
    int timeout_ms = 1000;
    int retries = 2;
    // Devices that tunnel the serial protocol over TCP need the RTU frame with
    // its CRC instead of the MBAP header.
    bool rtu_over_tcp = false;
};

class ModbusMaster {
public:
    explicit ModbusMaster(MasterConfig config);

    bool ensure_connected(std::string& error);
    void disconnect();
    bool connected() const { return socket_.valid(); }

    bool read(std::uint8_t function, std::uint16_t address, std::uint16_t count,
              modbus::ReadResponse& response, std::string& error);

    bool write_single_coil(std::uint16_t address, bool on, std::string& error);
    bool write_single_register(std::uint16_t address, std::uint16_t value, std::string& error);

    // How many transactions in a row have failed; the tag engine uses this to
    // decide when a device is really gone rather than just slow.
    int consecutive_failures() const { return consecutive_failures_; }
    const MasterConfig& config() const { return config_; }

private:
    bool transact(const modbus::Bytes& request, modbus::Bytes& response, std::string& error);
    bool transact_rtu(const modbus::Bytes& request, modbus::Bytes& response, std::string& error);

    MasterConfig config_;
    TcpSocket socket_;
    std::uint16_t transaction_ = 1;
    int consecutive_failures_ = 0;
};

}  // namespace scada
