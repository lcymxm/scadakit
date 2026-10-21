// A small simulated substation used as the "field device" side of the demo. It
// exists so the gateway has something realistic to poll when there is no PLC on
// the bench; the physics is only a first-order lag plus a sine, which is enough
// to make the HMI trends look like a real plant.
#pragma once

#include <cstdint>
#include <random>
#include <vector>

namespace scada {

// The register map is fixed and documented in README.md. Offsets are zero based;
// the 3xxxx / 4xxxx numbers in the README are just the human-facing form.
namespace plant_map {

// Input registers (3x): measurements, read only.
constexpr std::uint16_t kOilTemperature = 0;   // 0.1 degC
constexpr std::uint16_t kBusVoltage = 1;       // 0.1 kV
constexpr std::uint16_t kActivePower = 2;      // 0.1 MW
constexpr std::uint16_t kCurrent = 3;          // 0.1 A
constexpr std::uint16_t kPowerFactor = 4;      // 0.01
constexpr std::uint16_t kFeeder1Power = 5;     // 0.1 MW
constexpr std::uint16_t kFeeder2Power = 6;     // 0.1 MW
constexpr std::uint16_t kAmbientTemperature = 7;  // 0.1 degC
constexpr std::uint16_t kEnergy = 8;           // 0.1 kWh, 32-bit, high word first
constexpr std::uint16_t kInputRegisterCount = 10;

// Holding registers (4x): setpoints, read/write.
constexpr std::uint16_t kOilTemperatureSetpoint = 0;  // 0.1 degC
constexpr std::uint16_t kVoltageHighLimit = 1;        // 0.1 kV
constexpr std::uint16_t kVoltageLowLimit = 2;         // 0.1 kV
constexpr std::uint16_t kHoldingRegisterCount = 8;

// Coils (0x): remote control, read/write.
constexpr std::uint16_t kFeeder1Close = 0;
constexpr std::uint16_t kFeeder2Close = 1;
constexpr std::uint16_t kCoilCount = 8;

// Discrete inputs (1x): status, read only.
constexpr std::uint16_t kFeeder1Closed = 0;
constexpr std::uint16_t kFeeder2Closed = 1;
constexpr std::uint16_t kFanRunning = 2;
constexpr std::uint16_t kCommonAlarm = 3;
constexpr std::uint16_t kDiscreteInputCount = 8;

}  // namespace plant_map

class SimulatedPlant {
public:
    explicit SimulatedPlant(std::uint32_t seed = 1);

    // Advances the process by the time elapsed since the previous call.
    void tick(std::int64_t now_ms);

    std::vector<std::uint16_t>& input_registers() { return input_; }
    std::vector<std::uint16_t>& holding_registers() { return holding_; }
    std::vector<bool>& coils() { return coils_; }
    std::vector<bool>& discrete_inputs() { return discrete_; }
    const std::vector<std::uint16_t>& input_registers() const { return input_; }
    const std::vector<std::uint16_t>& holding_registers() const { return holding_; }
    const std::vector<bool>& coils() const { return coils_; }
    const std::vector<bool>& discrete_inputs() const { return discrete_; }

private:
    void recompute_measurements();

    std::mt19937 rng_;
    std::int64_t last_tick_ms_ = 0;

    std::vector<std::uint16_t> input_;
    std::vector<std::uint16_t> holding_;
    std::vector<bool> coils_;
    std::vector<bool> discrete_;

    // Process state, kept in engineering units.
    double oil_temperature_ = 45.0;
    double voltage_ = 10.4;
    double ambient_ = 25.0;
    double feeder1_mw_ = 8.0;
    double feeder2_mw_ = 6.5;
    double energy_kwh_ = 0.0;
    double sim_seconds_ = 0.0;
};

}  // namespace scada
