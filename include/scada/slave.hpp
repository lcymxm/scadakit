// Modbus server side: a pure request/response function over a register bank, plus
// a tiny TCP server that wraps it. Splitting the two means the protocol logic can
// be tested without opening a socket at all.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "scada/modbus.hpp"
#include "scada/plant.hpp"
#include "scada/socket.hpp"

namespace scada {

// The four Modbus data tables.
struct RegisterBank {
    std::uint8_t station = 1;
    std::vector<std::uint16_t> holding;
    std::vector<std::uint16_t> input;
    std::vector<bool> coils;
    std::vector<bool> discrete;
};

RegisterBank make_bank(const SimulatedPlant& plant);
void apply_bank(SimulatedPlant& plant, const RegisterBank& bank);

// Turns one request PDU into one response PDU. Returns an empty vector when the
// bytes cannot be a Modbus request at all, which the server treats as "drop the
// connection".
modbus::Bytes process_pdu(RegisterBank& bank, const modbus::Bytes& request);

class ModbusSlaveServer {
public:
    ModbusSlaveServer(SimulatedPlant& plant, std::uint8_t station = 1);
    ~ModbusSlaveServer();

    ModbusSlaveServer(const ModbusSlaveServer&) = delete;
    ModbusSlaveServer& operator=(const ModbusSlaveServer&) = delete;

    bool start(const std::string& address, std::uint16_t port, std::string& error);
    void stop();

    std::uint16_t port() const { return listener_.local_port(); }
    std::uint64_t requests_served() const { return requests_served_.load(); }

private:
    void run();

    SimulatedPlant& plant_;
    std::uint8_t station_;
    TcpListener listener_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> requests_served_{0};
};

}  // namespace scada
