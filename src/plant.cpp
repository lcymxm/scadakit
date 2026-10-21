#include "scada/plant.hpp"

#include <algorithm>
#include <cmath>

namespace scada {
namespace {

std::uint16_t to_register(double value) {
    const double clamped = std::clamp(value, 0.0, 65535.0);
    return static_cast<std::uint16_t>(std::lround(clamped));
}

}  // namespace

SimulatedPlant::SimulatedPlant(std::uint32_t seed) : rng_(seed) {
    input_.assign(plant_map::kInputRegisterCount, 0);
    holding_.assign(plant_map::kHoldingRegisterCount, 0);
    coils_.assign(plant_map::kCoilCount, false);
    discrete_.assign(plant_map::kDiscreteInputCount, false);

    holding_[plant_map::kOilTemperatureSetpoint] = 750;  // 75.0 degC
    holding_[plant_map::kVoltageHighLimit] = 108;        // 10.8 kV
    holding_[plant_map::kVoltageLowLimit] = 100;         // 10.0 kV

    // Both feeders start closed.
    coils_[plant_map::kFeeder1Close] = true;
    coils_[plant_map::kFeeder2Close] = true;
    discrete_[plant_map::kFeeder1Closed] = true;
    discrete_[plant_map::kFeeder2Closed] = true;

    recompute_measurements();
}

void SimulatedPlant::tick(std::int64_t now_ms) {
    if (last_tick_ms_ == 0) {
        last_tick_ms_ = now_ms;
    }
    double dt = static_cast<double>(now_ms - last_tick_ms_) / 1000.0;
    last_tick_ms_ = now_ms;
    // A long pause (a debugger break, a suspended process) must not send the
    // process model flying off to a nonsense state.
    dt = std::clamp(dt, 0.0, 2.0);

    sim_seconds_ += dt;

    std::uniform_real_distribution<double> noise(-1.0, 1.0);

    // Load follows a slow sine plus a little noise, and stops when the feeder
    // breaker is open.
    const double day_cycle = std::sin(sim_seconds_ * 2.0 * 3.14159265358979 / 600.0);
    feeder1_mw_ = coils_[plant_map::kFeeder1Close] ? 8.0 + 2.5 * day_cycle + 0.15 * noise(rng_)
                                                   : 0.0;
    feeder2_mw_ = coils_[plant_map::kFeeder2Close]
                      ? 6.5 + 2.0 * day_cycle * 0.8 + 0.15 * noise(rng_)
                      : 0.0;

    voltage_ = 10.45 + 0.12 * std::sin(sim_seconds_ * 2.0 * 3.14159265358979 / 240.0) +
               0.01 * noise(rng_);
    ambient_ = 25.0 + 3.0 * std::sin(sim_seconds_ * 2.0 * 3.14159265358979 / 900.0);

    // Oil temperature chases the load with a first-order lag; that is why the
    // trend on the HMI trails the power trend instead of tracking it exactly.
    const double load = feeder1_mw_ + feeder2_mw_;
    const double target = ambient_ + 6.0 + load * 1.6;
    const double tau = 45.0;  // seconds
    oil_temperature_ += (target - oil_temperature_) * std::min(dt / tau, 1.0);

    energy_kwh_ += load * dt / 3600.0 * 1000.0;

    // The fan starts a little above the setpoint and stops a little below it.
    const double setpoint = holding_[plant_map::kOilTemperatureSetpoint] / 10.0;
    if (!discrete_[plant_map::kFanRunning] && oil_temperature_ > setpoint) {
        discrete_[plant_map::kFanRunning] = true;
    } else if (discrete_[plant_map::kFanRunning] && oil_temperature_ < setpoint - 3.0) {
        discrete_[plant_map::kFanRunning] = false;
    }

    // Breaker position follows the remote control command.
    discrete_[plant_map::kFeeder1Closed] = coils_[plant_map::kFeeder1Close];
    discrete_[plant_map::kFeeder2Closed] = coils_[plant_map::kFeeder2Close];

    recompute_measurements();
}

void SimulatedPlant::recompute_measurements() {
    const double load = feeder1_mw_ + feeder2_mw_;
    const double power_factor = load > 0.05 ? 0.93 : 0.0;

    // I = P / (sqrt(3) * U * cos(phi)), with P in MW and U in kV giving kA.
    const double current_a =
        (load > 0.05 && voltage_ > 0.1) ? load / (std::sqrt(3.0) * voltage_ * power_factor) * 1000.0
                                        : 0.0;

    input_[plant_map::kOilTemperature] = to_register(oil_temperature_ * 10.0);
    input_[plant_map::kBusVoltage] = to_register(voltage_ * 10.0);
    input_[plant_map::kActivePower] = to_register(load * 10.0);
    input_[plant_map::kCurrent] = to_register(current_a * 10.0);
    input_[plant_map::kPowerFactor] = to_register(power_factor * 100.0);
    input_[plant_map::kFeeder1Power] = to_register(feeder1_mw_ * 10.0);
    input_[plant_map::kFeeder2Power] = to_register(feeder2_mw_ * 10.0);
    input_[plant_map::kAmbientTemperature] = to_register(ambient_ * 10.0);

    // 32-bit energy counter, high word first, in 0.1 kWh.
    const std::uint32_t energy = static_cast<std::uint32_t>(energy_kwh_ * 10.0);
    input_[plant_map::kEnergy] = static_cast<std::uint16_t>((energy >> 16) & 0xFFFF);
    input_[plant_map::kEnergy + 1] = static_cast<std::uint16_t>(energy & 0xFFFF);

    discrete_[plant_map::kCommonAlarm] =
        static_cast<double>(input_[plant_map::kOilTemperature]) / 10.0 >
        holding_[plant_map::kOilTemperatureSetpoint] / 10.0;
}

}  // namespace scada
