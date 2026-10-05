// Demo Modbus slave (TCP + UDP).
//   input area  (PLC -> PC, holding registers / coils)        printed whenever the master changes it
//   output area (PC -> PLC, input registers / discrete inputs) = echo of the input area,
//                                                                bytes 0..3 replaced by a heartbeat counter
//
// Usage: mb_slave_demo [--bind IP] [--port N] [--unit N] [--in-size N] [--out-size N]
//                      [--no-tcp 1] [--no-udp 1] [--outputs-in-holding-at N]
#include "softmb/modbus_slave.hpp"

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
    softmb::ModbusSlaveConfig cfg;
    cfg.unitId = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        unsigned long n = std::strtoul(v, nullptr, 0);
        if (k == "--bind")
            cfg.bindAddress = v;
        else if (k == "--port")
            cfg.port = uint16_t(n);
        else if (k == "--unit")
            cfg.unitId = uint8_t(n);
        else if (k == "--in-size")
            cfg.inputSize = n;
        else if (k == "--out-size")
            cfg.outputSize = n;
        else if (k == "--no-tcp")
            cfg.enableTcp = n == 0;
        else if (k == "--no-udp")
            cfg.enableUdp = n == 0;
        else if (k == "--outputs-in-holding-at")
            cfg.outputsInHoldingAt = int(n);
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }

    cfg.onLog = logLine;
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

    softmb::ModbusSlave slave(cfg);
    std::string error;
    if (!slave.start(&error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    logLine("unit id " + (cfg.unitId ? std::to_string(cfg.unitId) : std::string("any")) + ": holding registers 0.." +
            std::to_string((cfg.inputSize + 1) / 2 - 1) + " = inputs, input registers 0.." +
            std::to_string((cfg.outputSize + 1) / 2 - 1) + " = outputs. Ctrl+C to quit.");

    std::vector<uint8_t> in(cfg.inputSize), out(cfg.outputSize, 0);
    uint32_t heartbeat = 0;
    while (!g_quit) {
        slave.ioRead(0, in.data(), in.size());
        std::copy_n(in.begin(), std::min(in.size(), out.size()), out.begin());
        ++heartbeat;
        for (size_t i = 0; i < 4 && i < out.size(); ++i)
            out[i] = uint8_t(heartbeat >> (8 * i));
        slave.ioWrite(0, out.data(), out.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    slave.stop();
    logLine("stopped");
    return 0;
}
