#include "scada/slave.hpp"

#include "scada/types.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace scada {
namespace {

using modbus::Bytes;

void put_u16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t get_u16(const Bytes& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) |
                                      bytes[offset + 1]);
}

Bytes exception_response(std::uint8_t function, modbus::Exception code) {
    return Bytes{static_cast<std::uint8_t>(function | 0x80), static_cast<std::uint8_t>(code)};
}

// Packs a run of bits into ceil(n/8) bytes, least significant bit first.
Bytes pack_bits(const std::vector<bool>& bits, std::size_t address, std::size_t count) {
    Bytes packed((count + 7) / 8, 0);
    for (std::size_t i = 0; i < count; ++i) {
        if (bits[address + i]) {
            packed[i / 8] = static_cast<std::uint8_t>(packed[i / 8] | (1u << (i % 8)));
        }
    }
    return packed;
}

}  // namespace

RegisterBank make_bank(const SimulatedPlant& plant) {
    RegisterBank bank;
    bank.holding = plant.holding_registers();
    bank.input = plant.input_registers();
    bank.coils = plant.coils();
    bank.discrete = plant.discrete_inputs();
    return bank;
}

void apply_bank(SimulatedPlant& plant, const RegisterBank& bank) {
    plant.holding_registers() = bank.holding;
    plant.coils() = bank.coils;
}

Bytes process_pdu(RegisterBank& bank, const Bytes& request) {
    if (request.empty()) {
        return {};
    }
    const std::uint8_t function = request[0];

    switch (function) {
        case modbus::kReadCoils:
        case modbus::kReadDiscreteInputs: {
            if (request.size() != 5) {
                return exception_response(function, modbus::Exception::IllegalDataValue);
            }
            const std::uint16_t address = get_u16(request, 1);
            const std::uint16_t count = get_u16(request, 3);
            if (count == 0 || count > modbus::kMaxReadBits) {
                return exception_response(function, modbus::Exception::IllegalDataValue);
            }
            const std::vector<bool>& source =
                function == modbus::kReadCoils ? bank.coils : bank.discrete;
            if (static_cast<std::size_t>(address) + count > source.size()) {
                return exception_response(function, modbus::Exception::IllegalDataAddress);
            }
            const Bytes packed = pack_bits(source, address, count);
            Bytes response;
            response.reserve(2 + packed.size());
            response.push_back(function);
            response.push_back(static_cast<std::uint8_t>(packed.size()));
            response.insert(response.end(), packed.begin(), packed.end());
            return response;
        }

        case modbus::kReadHoldingRegisters:
        case modbus::kReadInputRegisters: {
            if (request.size() != 5) {
                return exception_response(function, modbus::Exception::IllegalDataValue);
            }
            const std::uint16_t address = get_u16(request, 1);
            const std::uint16_t count = get_u16(request, 3);
            if (count == 0 || count > modbus::kMaxReadRegisters) {
                return exception_response(function, modbus::Exception::IllegalDataValue);
            }
            const std::vector<std::uint16_t>& source =
                function == modbus::kReadHoldingRegisters ? bank.holding : bank.input;
            if (static_cast<std::size_t>(address) + count > source.size()) {
                return exception_response(function, modbus::Exception::IllegalDataAddress);
            }
            Bytes response;
            response.reserve(2 + 2 * count);
            response.push_back(function);
            response.push_back(static_cast<std::uint8_t>(count * 2));
            for (std::uint16_t i = 0; i < count; ++i) {
                put_u16(response, source[address + i]);
            }
            return response;
        }

        case modbus::kWriteSingleCoil: {
            if (request.size() != 5) {
                return exception_response(function, modbus::Exception::IllegalDataValue);
            }
            const std::uint16_t address = get_u16(request, 1);
            const std::uint16_t value = get_u16(request, 3);
            if (value != 0xFF00 && value != 0x0000) {
                return exception_response(function, modbus::Exception::IllegalDataValue);
            }
            if (address >= bank.coils.size()) {
                return exception_response(function, modbus::Exception::IllegalDataAddress);
            }
            bank.coils[address] = value == 0xFF00;
            return request;  // the response echoes the request
        }

        case modbus::kWriteSingleRegister: {
            if (request.size() != 5) {
                return exception_response(function, modbus::Exception::IllegalDataValue);
            }
            const std::uint16_t address = get_u16(request, 1);
            if (address >= bank.holding.size()) {
                return exception_response(function, modbus::Exception::IllegalDataAddress);
            }
            bank.holding[address] = get_u16(request, 3);
            return request;
        }

        default:
            return exception_response(function, modbus::Exception::IllegalFunction);
    }
}

ModbusSlaveServer::ModbusSlaveServer(SimulatedPlant& plant, std::uint8_t station)
    : plant_(plant), station_(station) {}

ModbusSlaveServer::~ModbusSlaveServer() { stop(); }

bool ModbusSlaveServer::start(const std::string& address, std::uint16_t port,
                              std::string& error) {
    if (running_.load()) {
        error = "server already running";
        return false;
    }
    listener_ = TcpListener::bind_to(address, port, error);
    if (!listener_.valid()) {
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&ModbusSlaveServer::run, this);
    return true;
}

void ModbusSlaveServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    listener_.close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ModbusSlaveServer::run() {
    // Several clients at once, each with its own partial-frame buffer. A real PLC
    // may allow only one connection, but refusing the second client just because
    // the HMI is already polling makes the simulator annoying to work with.
    struct Connection {
        TcpSocket socket;
        Bytes pending;
    };
    std::vector<Connection> clients;

    std::uint8_t buffer[512];

    while (running_.load()) {
        {
            TcpSocket fresh = listener_.accept(20);
            if (fresh.valid()) {
                clients.push_back(Connection{std::move(fresh), Bytes{}});
            }
        }

        for (std::size_t c = 0; c < clients.size();) {
            Connection& connection = clients[c];

            if (!connection.socket.valid()) {
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(c));
                continue;
            }

            // Drain whatever has arrived without blocking.
            while (connection.socket.wait_readable(0)) {
                const int received = connection.socket.receive(buffer, sizeof(buffer), 0);
                if (received <= 0) {
                    connection.socket.close();
                    break;
                }
                connection.pending.insert(connection.pending.end(), buffer,
                                          buffer + received);
            }
            if (!connection.socket.valid()) {
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(c));
                continue;
            }

            // Serve every complete frame that is now buffered.
            bool keep = true;
            while (keep) {
                if (connection.pending.size() < modbus::kMbapHeaderSize) {
                    break;
                }
                const std::size_t total =
                    modbus::mbap_frame_length(connection.pending.data(), connection.pending.size());
                if (total < modbus::kMbapHeaderSize || total > 260) {
                    keep = false;
                    break;
                }
                if (connection.pending.size() < total) {
                    break;  // wait for the rest
                }

                modbus::MbapFrame mbap;
                if (!modbus::unwrap_mbap(connection.pending.data(), total, mbap)) {
                    keep = false;
                    break;
                }

                if (mbap.unit == station_ || mbap.unit == 0) {
                    plant_.tick(now_ms());
                    RegisterBank bank = make_bank(plant_);
                    const Bytes response = process_pdu(bank, mbap.pdu);
                    if (response.empty()) {
                        keep = false;
                        break;
                    }
                    apply_bank(plant_, bank);

                    const Bytes out = modbus::wrap_mbap(mbap.transaction, station_, response);
                    if (connection.socket.send_bytes(out.data(), out.size()) < 0) {
                        keep = false;
                        break;
                    }
                    requests_served_.fetch_add(1);
                }
                connection.pending.erase(connection.pending.begin(),
                                         connection.pending.begin() +
                                             static_cast<std::ptrdiff_t>(total));
            }

            if (!keep) {
                connection.socket.close();
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(c));
                continue;
            }
            ++c;
        }
    }
}

}  // namespace scada
