#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "scada/alarm.hpp"
#include "scada/dashboard.hpp"
#include "scada/gateway.hpp"
#include "scada/http.hpp"
#include "scada/plant.hpp"
#include "scada/slave.hpp"
#include "scada/tag.hpp"

namespace {

constexpr const char* kVersion = "1.0.0";

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) { g_stop = 1; }

void print_usage() {
    std::cout << R"(ScadaKit - a small SCADA gateway

usage:
  scadakit <command> [options]

commands:
  simulate   run the PLC simulator: a Modbus TCP slave with a small substation
  run        run the gateway, the alarm engine and the web HMI
  probe      read every tag once and print the result, then exit

options:
  --tags <file>       tag configuration (default config/example.tags)
  --host <address>    device address (default 127.0.0.1)
  --port <port>       device Modbus port (default 1502)
  --web-port <port>   HMI port (default 8080)
  --bind <address>    HMI bind address (default 127.0.0.1)
  --poll <ms>         polling period in milliseconds (default 500)
  --history <count>   trend points kept per tag (default 900)
  --rtu               use RTU framing over TCP instead of MBAP
  -h, --help          show this help
  --version           show the version

examples:
  scadakit simulate --port 1502
  scadakit run --tags config/example.tags --web-port 8080
)";
}

struct Options {
    std::string command;
    std::string tag_file = "config/example.tags";
    std::string host = "127.0.0.1";
    std::string bind = "127.0.0.1";
    std::uint16_t port = 1502;
    std::uint16_t web_port = 8080;
    int poll_ms = 500;
    std::size_t history = 900;
    bool rtu = false;
};

bool parse_options(int argc, char** argv, Options& options) {
    if (argc < 2) {
        print_usage();
        return false;
    }
    options.command = argv[1];
    if (options.command == "--help" || options.command == "-h") {
        print_usage();
        std::exit(0);
    }
    if (options.command == "--version") {
        std::cout << "scadakit " << kVersion << "\n";
        std::exit(0);
    }

    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " needs a value\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (argument == "--tags") {
            options.tag_file = next("--tags");
        } else if (argument == "--host") {
            options.host = next("--host");
        } else if (argument == "--bind") {
            options.bind = next("--bind");
        } else if (argument == "--port") {
            options.port = static_cast<std::uint16_t>(std::stoi(next("--port")));
        } else if (argument == "--web-port") {
            options.web_port = static_cast<std::uint16_t>(std::stoi(next("--web-port")));
        } else if (argument == "--poll") {
            options.poll_ms = std::stoi(next("--poll"));
        } else if (argument == "--history") {
            options.history = static_cast<std::size_t>(std::stoul(next("--history")));
        } else if (argument == "--rtu") {
            options.rtu = true;
        } else if (argument == "--help" || argument == "-h") {
            print_usage();
            std::exit(0);
        } else {
            std::cerr << "error: unknown option '" << argument << "'\n";
            return false;
        }
    }
    return true;
}

// Terminal column width: CJK characters occupy two cells, and the tag
// descriptions in the example configuration are Chinese.
std::size_t display_width(const std::string& text) {
    std::size_t width = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) {
            width += 1;
            i += 1;
        } else if ((byte >> 5) == 0x6) {
            width += 1;
            i += 2;
        } else if ((byte >> 4) == 0xE) {
            width += 2;
            i += 3;
        } else {
            width += 2;
            i += 4;
        }
    }
    return width;
}

std::string pad(const std::string& text, std::size_t width) {
    const std::size_t current = display_width(text);
    return current >= width ? text : text + std::string(width - current, ' ');
}

std::string pad_left(const std::string& text, std::size_t width) {
    const std::size_t current = display_width(text);
    return current >= width ? text : std::string(width - current, ' ') + text;
}

std::string quality_mark(scada::Quality quality) {
    switch (quality) {
        case scada::Quality::Good:
            return "OK  ";
        case scada::Quality::Uncertain:
            return "UNC ";
        case scada::Quality::Bad:
            return "BAD ";
        case scada::Quality::Stale:
            return "STALE";
        case scada::Quality::CommFailure:
            return "COMM ";
    }
    return "?    ";
}

int command_simulate(const Options& options) {
    scada::SimulatedPlant plant(1);
    scada::ModbusSlaveServer server(plant, 1);

    std::string error;
    if (!server.start(options.bind, options.port, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    std::cout << "PLC simulator listening on " << options.bind << ":" << server.port() << "\n";
    std::cout << "press Ctrl+C to stop\n\n";
    std::cout << "register map (see config/example.tags):\n";
    std::cout << "  input registers  3x0001..  oil temperature, voltage, power, ...\n";
    std::cout << "  holding registers 4x0001.. setpoints\n";
    std::cout << "  coils            0x0001..  feeder close commands\n";
    std::cout << "  discrete inputs  1x0001..  breaker positions, fan, common alarm\n";

    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server.stop();
    std::cout << "\nstopped after " << server.requests_served() << " requests\n";
    return 0;
}

int command_probe(const Options& options) {
    scada::TagTable tags = scada::TagTable::load(options.tag_file);
    scada::GatewayConfig config;
    config.master.host = options.host;
    config.master.port = options.port;
    config.master.rtu_over_tcp = options.rtu;
    config.history_capacity = options.history;

    scada::Gateway gateway(std::move(tags), config);
    gateway.poll_once(scada::now_ms());

    std::cout << pad("tag", 12) << ' ' << pad("description", 26) << ' ' << pad_left("value", 12)
              << ' ' << pad("unit", 5) << ' ' << pad("quality", 11) << "alarm\n";
    std::cout << std::string(80, '-') << '\n';
    for (std::size_t i = 0; i < gateway.database().size(); ++i) {
        const scada::TagDefinition& tag = gateway.database().definition(i);
        const scada::TagValue& value = gateway.database().value(i);
        scada::AlarmState worst = scada::AlarmState::Normal;
        for (std::size_t level = 0; level < scada::kAlarmLevelCount; ++level) {
            const scada::AlarmState state =
                gateway.alarms().state(i, static_cast<scada::AlarmLevel>(level));
            if (state != scada::AlarmState::Normal) {
                worst = state;
                break;
            }
        }
        std::ostringstream line;
        line << std::fixed << std::setprecision(3) << value.value;
        std::cout << pad(tag.name, 12) << ' ' << pad(tag.description, 26) << ' '
                  << pad_left(line.str(), 12) << ' ' << pad(tag.unit, 5) << ' '
                  << pad(quality_mark(value.quality), 11) << scada::to_string(worst) << '\n';
    }
    if (!gateway.device_online()) {
        std::cerr << "\nwarning: no response from the device (" << gateway.last_error() << ")\n";
        return 2;
    }
    return 0;
}

int command_run(const Options& options) {
    scada::TagTable tags = scada::TagTable::load(options.tag_file);
    scada::GatewayConfig config;
    config.master.host = options.host;
    config.master.port = options.port;
    config.master.rtu_over_tcp = options.rtu;
    config.poll_interval_ms = options.poll_ms;
    config.history_capacity = options.history;

    scada::Gateway gateway(std::move(tags), config);

    scada::HttpServer server;
    server.set_handler([&gateway](const scada::HttpRequest& request) -> scada::HttpResponse {
        if (request.path == "/" || request.path == "/index.html") {
            return scada::HttpResponse::html(scada::dashboard_html());
        }
        if (request.path == "/api/snapshot") {
            return scada::HttpResponse::json(gateway.snapshot_json());
        }
        if (request.path == "/api/alarms") {
            return scada::HttpResponse::json(gateway.alarms_json());
        }
        if (request.path == "/api/history") {
            const std::string name = request.query_value("tag");
            const std::size_t index = gateway.database().index_of(name);
            if (index >= gateway.database().size()) {
                return scada::HttpResponse::text("unknown tag", 404);
            }
            return scada::HttpResponse::json(gateway.history_json(index));
        }
        if (request.path == "/api/ack") {
            const std::string name = request.query_value("tag");
            if (request.method != "POST") {
                return scada::HttpResponse::text("POST required", 405);
            }
            const std::size_t acknowledged = gateway.acknowledge(name);
            return scada::HttpResponse::json("{\"acknowledged\":" +
                                             std::to_string(acknowledged) + "}");
        }
        return scada::HttpResponse::text("not found", 404);
    });

    std::string error;
    if (!server.start(options.bind, options.web_port, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    std::cout << "ScadaKit " << kVersion << "\n";
    std::cout << "  tags     : " << options.tag_file << " (" << gateway.tags().size()
              << " points, " << gateway.blocks().size() << " read blocks)\n";
    std::cout << "  device   : " << options.host << ":" << options.port
              << (options.rtu ? " (RTU over TCP)" : " (Modbus TCP)") << "\n";
    std::cout << "  HMI      : http://" << options.bind << ":" << server.port() << "/\n";
    std::cout << "  polling  : every " << options.poll_ms << " ms\n";
    std::cout << "  Ctrl+C to stop\n\n";

    while (g_stop == 0) {
        gateway.poll_once(scada::now_ms());
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }

    server.stop();
    std::cout << "\nstopped after " << gateway.poll_count() << " scans, "
              << server.requests_served() << " HTTP requests\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    Options options;
    if (!parse_options(argc, argv, options)) {
        return 1;
    }

    try {
        if (options.command == "simulate") {
            return command_simulate(options);
        }
        if (options.command == "run") {
            return command_run(options);
        }
        if (options.command == "probe") {
            return command_probe(options);
        }
        std::cerr << "error: unknown command '" << options.command << "'\n\n";
        print_usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
