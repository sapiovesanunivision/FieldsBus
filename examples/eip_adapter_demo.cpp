// Demo EtherNet/IP device.
//   inputs (to PLC)  = echo of the outputs, bytes 0..3 replaced by a heartbeat counter
//   outputs (from PLC) are printed whenever they change
//
// Usage: eip_adapter_demo [--bind IP] [--in-size N] [--out-size N] [--min-rpi-us N] [--name TEXT]
#include "softeip/eip_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include "softeip/periodic_timer.hpp"

namespace {

std::atomic<bool> g_quit{false};

using softeip::PeriodicTimer;

void onSignal(int) { g_quit = true; }

void logLine(const std::string& msg)
{
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tmLocal{};
#ifdef _WIN32
    localtime_s(&tmLocal, &t);   // MSVC: std::localtime is C4996 (deprecated, not thread-safe)
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
    softeip::AdapterConfig cfg;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--bind")
            cfg.bindAddress = v;
        else if (k == "--in-size")
            cfg.inputSize = std::strtoul(v, nullptr, 0);
        else if (k == "--out-size")
            cfg.outputSize = std::strtoul(v, nullptr, 0);
        else if (k == "--min-rpi-us")
            cfg.minRpiUs = std::strtoul(v, nullptr, 0);
        else if (k == "--name")
            cfg.identity.productName = v;
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }

    cfg.onLog = logLine;
    cfg.onConnectionChanged = [](bool connected) {
        logLine(connected ? "PLC connected (exclusive owner)" : "PLC disconnected");
    };
    cfg.onOutputs = [](const std::vector<uint8_t>& out, bool run) {
        std::string line = std::string("outputs [") + (run ? "RUN " : "IDLE") + "]";
        for (size_t i = 0; i < out.size() && i < 16; ++i) {
            char b[4];
            std::snprintf(b, sizeof b, " %02X", out[i]);
            line += b;
        }
        if (out.size() > 16)
            line += " ...";
        logLine(line);
    };

    softeip::Adapter adapter(cfg);
    std::string error;
    if (!adapter.start(&error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    logLine("input " + std::to_string(cfg.inputSize) + " B (instance " + std::to_string(cfg.inputInstance) +
            "), output " + std::to_string(cfg.outputSize) + " B (instance " + std::to_string(cfg.outputInstance) +
            "), config instance " + std::to_string(cfg.configInstance) + ". Ctrl+C to quit.");

    // Application cycle: fixed 10 ms period (see PeriodicTimer), so the work and any
    // oversleep don't stretch the cycle the way sleep_for(10 ms) after the work did.
    PeriodicTimer cycle(std::chrono::milliseconds(10));
    logLine(std::string("application cycle 10 ms via ") + cycle.kind());
    std::vector<uint8_t> inputs(cfg.inputSize, 0);
    uint32_t heartbeat = 0;
    while (!g_quit) {
        std::vector<uint8_t> outputs = adapter.outputData();
        std::memcpy(inputs.data(), outputs.data(), std::min(inputs.size(), outputs.size()));
        ++heartbeat;
        for (size_t i = 0; i < 4 && i < inputs.size(); ++i)
            inputs[i] = uint8_t(heartbeat >> (8 * i));
        adapter.setInputData(inputs.data(), inputs.size());
        cycle.wait();
    }

    adapter.stop();
    logLine("stopped");
    return 0;
}
