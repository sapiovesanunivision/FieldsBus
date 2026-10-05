// Same application on any fieldbus: the transport is just a command-line option.
//   input area  (PLC -> PC)  printed whenever the PLC changes it
//   output area (PC -> PLC)  = echo of the input area, bytes 0..3 replaced by a heartbeat counter
//
// Usage: fb_device_demo --transport eip|modbus-tcp|modbus-udp|modbus
//                       [--bind IP] [--in-size N] [--out-size N] [--port N] [--unit N]
#include "softfb/fieldbus_device.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_quit{false};

void onSignal(int) { g_quit = true; }

void logLine(const std::string& msg)
{
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tmLocal{};
#ifdef _WIN32
    localtime_s(&tmLocal, &t);
#else
    localtime_r(&t, &tmLocal);
#endif
    char ts[16];
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tmLocal);
    std::printf("%s.%03d %s\n", ts, int(ms % 1000), msg.c_str());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    softfb::DeviceConfig cfg;
    cfg.inputSize = 64;
    cfg.outputSize = 64;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        unsigned long n = std::strtoul(v, nullptr, 0);
        if (k == "--transport") {
            if (!softfb::FieldbusDevice::parseTransport(v, cfg.transport)) {
                std::fprintf(stderr, "unknown transport %s\n", v);
                return 2;
            }
        } else if (k == "--bind")
            cfg.bindAddress = v;
        else if (k == "--in-size")
            cfg.inputSize = n;
        else if (k == "--out-size")
            cfg.outputSize = n;
        else if (k == "--port")
            cfg.modbus.port = uint16_t(n);
        else if (k == "--unit")
            cfg.modbus.unitId = uint8_t(n);
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }

    cfg.onLog = logLine;
    cfg.onStateChanged = [](softfb::DeviceState s) {
        logLine(std::string("state: ") + softfb::FieldbusDevice::stateName(s));
    };
    cfg.onInputsChanged = [](const std::vector<uint8_t>& in) {
        std::string line = "inputs (PLC -> PC)";
        for (size_t i = 0; i < in.size() && i < 16; ++i) {
            char b[4];
            std::snprintf(b, sizeof b, " %02X", in[i]);
            line += b;
        }
        if (in.size() > 16)
            line += " ...";
        logLine(line);
    };

    softfb::FieldbusDevice dev(cfg);
    std::string error;
    if (!dev.start(&error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    logLine(std::string("transport ") + softfb::FieldbusDevice::transportName(cfg.transport) + ", input " +
            std::to_string(cfg.inputSize) + " B (PLC -> PC), output " + std::to_string(cfg.outputSize) +
            " B (PC -> PLC). Ctrl+C to quit.");

    // The application: identical for every transport.
    std::vector<uint8_t> in(cfg.inputSize), out(cfg.outputSize, 0);
    uint32_t heartbeat = 0;
    while (!g_quit) {
        dev.ioRead(0, in.data(), in.size());
        std::copy_n(in.begin(), std::min(in.size(), out.size()), out.begin());
        ++heartbeat;
        for (size_t i = 0; i < 4 && i < out.size(); ++i)
            out[i] = uint8_t(heartbeat >> (8 * i));
        dev.ioWrite(0, out.data(), out.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    dev.stop();
    logLine("stopped");
    return 0;
}
