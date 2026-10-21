#include <string>
#include <thread>
#include <vector>

#include "scada/master.hpp"
#include "scada/modbus.hpp"
#include "scada/types.hpp"
#include "scada/plant.hpp"
#include "scada/slave.hpp"
#include "test_framework.hpp"

using namespace scada;

TEST(Modbus, Crc16MatchesTheStandardCheckValue) {
    // The CRC-16/MODBUS check value for "123456789" is 0x4B37.
    const std::string input = "123456789";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    CHECK_EQ(modbus::crc16(bytes, input.size()), 0x4B37);
}

TEST(Modbus, Crc16OfARequestFrameIsStable) {
    // Build a read-holding-registers request and append its CRC: the CRC over the
    // complete frame including the CRC itself must come out as zero.
    modbus::Bytes frame = modbus::wrap_rtu(1, modbus::make_read_request(
                                                  modbus::kReadHoldingRegisters, 0, 2));
    CHECK_EQ(frame.size(), std::size_t{8});
    CHECK_EQ(modbus::crc16(frame), std::uint16_t{0});
}

TEST(Modbus, MbapRoundTrip) {
    const modbus::Bytes pdu = modbus::make_read_request(modbus::kReadInputRegisters, 0x10, 4);
    const modbus::Bytes frame = modbus::wrap_mbap(0x1234, 7, pdu);

    CHECK_EQ(frame.size(), modbus::kMbapHeaderSize + pdu.size());
    CHECK_EQ(modbus::mbap_frame_length(frame.data(), frame.size()), frame.size());

    modbus::MbapFrame parsed;
    CHECK(modbus::unwrap_mbap(frame.data(), frame.size(), parsed));
    CHECK_EQ(parsed.transaction, std::uint16_t{0x1234});
    CHECK_EQ(parsed.unit, std::uint8_t{7});
    CHECK_EQ(parsed.pdu.size(), pdu.size());
    CHECK_EQ(parsed.pdu[0], std::uint8_t{modbus::kReadInputRegisters});
}

TEST(Modbus, MbapRejectsABadLengthField) {
    const modbus::Bytes frame = modbus::wrap_mbap(1, 1, modbus::make_read_request(3, 0, 1));
    modbus::Bytes truncated(frame.begin(), frame.end() - 1);
    modbus::MbapFrame parsed;
    CHECK(!modbus::unwrap_mbap(truncated.data(), truncated.size(), parsed));
}

TEST(Modbus, RtuRoundTripAndCrcRejection) {
    const modbus::Bytes pdu = modbus::make_read_request(modbus::kReadHoldingRegisters, 100, 3);
    modbus::Bytes frame = modbus::wrap_rtu(9, pdu);

    modbus::RtuFrame parsed;
    CHECK(modbus::unwrap_rtu(frame.data(), frame.size(), parsed));
    CHECK_EQ(parsed.unit, std::uint8_t{9});
    CHECK_EQ(parsed.pdu.size(), pdu.size());

    // Flip one payload bit; the CRC must reject it.
    frame[2] = static_cast<std::uint8_t>(frame[2] ^ 0x01);
    CHECK(!modbus::unwrap_rtu(frame.data(), frame.size(), parsed));
}

TEST(Modbus, ParsesRegisterResponse) {
    const std::uint8_t pdu[] = {modbus::kReadHoldingRegisters, 4, 0x00, 0x64, 0x01, 0x2C};
    modbus::ReadResponse response;
    CHECK(modbus::parse_read_response(modbus::kReadHoldingRegisters, pdu, sizeof(pdu), response));
    CHECK(!response.is_exception);
    CHECK_EQ(response.registers.size(), std::size_t{2});
    CHECK_EQ(response.registers[0], std::uint16_t{100});
    CHECK_EQ(response.registers[1], std::uint16_t{300});
}

TEST(Modbus, ParsesCoilResponseBitOrder) {
    // Two bytes of coils: 0b00000101 then 0b10000000. Bit 0 comes first, which is
    // the part everyone gets wrong the first time.
    const std::uint8_t pdu[] = {modbus::kReadCoils, 2, 0x05, 0x80};
    modbus::ReadResponse response;
    CHECK(modbus::parse_read_response(modbus::kReadCoils, pdu, sizeof(pdu), response));
    CHECK_EQ(response.bits.size(), std::size_t{16});
    CHECK(response.bits[0]);
    CHECK(!response.bits[1]);
    CHECK(response.bits[2]);
    CHECK(!response.bits[3]);
    CHECK(!response.bits[7]);
    CHECK(response.bits[15]);
}

TEST(Modbus, ParsesExceptionResponse) {
    const std::uint8_t pdu[] = {static_cast<std::uint8_t>(0x83), 0x02};
    modbus::ReadResponse response;
    CHECK(modbus::parse_read_response(modbus::kReadHoldingRegisters, pdu, sizeof(pdu), response));
    CHECK(response.is_exception);
    CHECK(response.exception == modbus::Exception::IllegalDataAddress);
}

TEST(Modbus, RejectsResponseWithWrongByteCount) {
    const std::uint8_t pdu[] = {modbus::kReadHoldingRegisters, 6, 0x00, 0x01};
    modbus::ReadResponse response;
    CHECK(!modbus::parse_read_response(modbus::kReadHoldingRegisters, pdu, sizeof(pdu), response));
}

TEST(Slave, ServesInputRegisters) {
    SimulatedPlant plant(1);
    plant.tick(now_ms());
    RegisterBank bank = make_bank(plant);

    const modbus::Bytes request =
        modbus::make_read_request(modbus::kReadInputRegisters, 0, 3);
    const modbus::Bytes response = process_pdu(bank, request);

    modbus::ReadResponse parsed;
    CHECK(modbus::parse_read_response(modbus::kReadInputRegisters, response.data(), response.size(),
                                      parsed));
    CHECK_EQ(parsed.registers.size(), std::size_t{3});

    // The bank was copied from the plant, so the bytes must decode back to the
    // same registers.
    CHECK_EQ(parsed.registers[plant_map::kOilTemperature],
             plant.input_registers()[plant_map::kOilTemperature]);
}

TEST(Slave, RejectsOutOfRangeAddress) {
    SimulatedPlant plant(1);
    RegisterBank bank = make_bank(plant);
    const modbus::Bytes request =
        modbus::make_read_request(modbus::kReadHoldingRegisters, 900, 2);
    const modbus::Bytes response = process_pdu(bank, request);

    CHECK_EQ(response.size(), std::size_t{2});
    CHECK_EQ(response[0], std::uint8_t{0x83});
    CHECK_EQ(response[1], std::uint8_t{0x02});
}

TEST(Slave, RejectsUnknownFunction) {
    SimulatedPlant plant(1);
    RegisterBank bank = make_bank(plant);
    const modbus::Bytes request{0x41, 0x00, 0x00};
    const modbus::Bytes response = process_pdu(bank, request);
    CHECK_EQ(response.size(), std::size_t{2});
    CHECK_EQ(response[0], std::uint8_t{0xC1});
    CHECK_EQ(response[1], std::uint8_t{0x01});
}

TEST(Slave, WriteCoilUpdatesTheBank) {
    SimulatedPlant plant(1);
    RegisterBank bank = make_bank(plant);
    const modbus::Bytes request = modbus::make_write_single_coil(plant_map::kFeeder2Close, false);
    const modbus::Bytes response = process_pdu(bank, request);

    CHECK_EQ(response.size(), request.size());  // echo
    CHECK(!bank.coils[plant_map::kFeeder2Close]);
}

TEST(Slave, RejectsInvalidCoilValue) {
    SimulatedPlant plant(1);
    RegisterBank bank = make_bank(plant);
    // 0x1234 is not one of the two legal coil values.
    const modbus::Bytes request{modbus::kWriteSingleCoil, 0x00, 0x00, 0x12, 0x34};
    const modbus::Bytes response = process_pdu(bank, request);
    CHECK_EQ(response[0], std::uint8_t{0x85});
    CHECK_EQ(response[1], std::uint8_t{0x03});
}

TEST(Slave, RejectsRegisterBlockThatOverrunsTheTable) {
    SimulatedPlant plant(1);
    RegisterBank bank = make_bank(plant);
    const modbus::Bytes request =
        modbus::make_read_request(modbus::kReadHoldingRegisters, 6, 10);
    const modbus::Bytes response = process_pdu(bank, request);
    CHECK_EQ(response[1], std::uint8_t{0x02});
}

TEST(Socket, LoopbackEcho) {
    std::string error;
    TcpListener listener = TcpListener::bind_to("127.0.0.1", 0, error);
    CHECK(listener.valid());
    CHECK(listener.local_port() != 0);

    std::thread server([&listener]() {
        TcpSocket client = listener.accept(2000);
        if (!client.valid()) {
            return;
        }
        std::uint8_t buffer[8];
        if (client.receive_exactly(buffer, 4, 2000)) {
            client.send_bytes(buffer, 4);
        }
    });

    TcpSocket socket = TcpSocket::connect_to("127.0.0.1", listener.local_port(), 2000, error);
    CHECK(socket.valid());
    const std::uint8_t payload[4] = {1, 2, 3, 4};
    CHECK_EQ(socket.send_bytes(payload, 4), 4);

    std::uint8_t echo[4];
    CHECK(socket.receive_exactly(echo, 4, 2000));
    CHECK_EQ(echo[3], std::uint8_t{4});
    server.join();
}

TEST(Socket, ConnectFailureIsReported) {
    std::string error;
    // Nothing is listening on this port, so the connect must fail cleanly instead
    // of hanging or crashing.
    TcpSocket socket = TcpSocket::connect_to("127.0.0.1", 1, 500, error);
    CHECK(!socket.valid());
    CHECK(!error.empty());
}

TEST(Master, ReadsTheSimulatedPlant) {
    SimulatedPlant plant(42);
    ModbusSlaveServer server(plant, 1);
    std::string error;
    CHECK(server.start("127.0.0.1", 0, error));
    CHECK(server.port() != 0);

    MasterConfig config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.station = 1;
    config.timeout_ms = 2000;
    config.retries = 1;
    ModbusMaster master(config);

    modbus::ReadResponse response;
    CHECK(master.read(modbus::kReadInputRegisters, 0, 8, response, error));
    CHECK(!response.is_exception);
    CHECK_EQ(response.registers.size(), std::size_t{8});

    // Voltage is around 10.4 kV, stored as 0.1 kV -> about 104.
    CHECK(response.registers[plant_map::kBusVoltage] > 90);
    CHECK(response.registers[plant_map::kBusVoltage] < 120);

    // The server bumps its counter right after writing the reply, so give it a
    // moment instead of racing it.
    for (int i = 0; i < 100 && server.requests_served() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(server.requests_served() > 0);

    server.stop();
}

TEST(Master, WriteCoilThenReadItBack) {
    SimulatedPlant plant(7);
    ModbusSlaveServer server(plant, 1);
    std::string error;
    CHECK(server.start("127.0.0.1", 0, error));

    MasterConfig config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.timeout_ms = 2000;
    config.retries = 1;
    ModbusMaster master(config);

    CHECK(master.write_single_coil(plant_map::kFeeder1Close, false, error));

    modbus::ReadResponse response;
    CHECK(master.read(modbus::kReadCoils, 0, 6, response, error));
    // Bits are packed into whole bytes, so asking for 6 coils yields all 8 bits of
    // the single returned byte. Callers must only use the first `count` of them.
    CHECK_EQ(response.bits.size(), std::size_t{8});
    CHECK(!response.bits[plant_map::kFeeder1Close]);

    server.stop();
}

TEST(Master, ReportsFailureWhenTheDeviceIsAbsent) {
    MasterConfig config;
    config.host = "127.0.0.1";
    config.port = 1;  // nothing listening
    config.timeout_ms = 300;
    config.retries = 0;
    ModbusMaster master(config);

    modbus::ReadResponse response;
    std::string error;
    CHECK(!master.read(modbus::kReadInputRegisters, 0, 2, response, error));
    CHECK(!error.empty());
    CHECK(master.consecutive_failures() > 0);
}
