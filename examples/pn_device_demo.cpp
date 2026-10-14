// Demo PROFINET IO-Device: echoes the PLC's outputs back as inputs, bytes 0..3 replaced by a
// heartbeat counter (the same application as eip_adapter_demo).
//
// Usage: pn_device_demo --adapter <name|mac|ip> [--name softpn-device] [--in-size 32] [--out-size 32]
//                       [--min-update-ms 8] [--no-vlan 1] [--promisc 0|1] [--priority 0|1] [--persist softpn.ini]
//        pn_device_demo --list
//
// In the PLC project: GSDML gsdml/GSDML-V2.43-Univision-SoftPN-*.xml, slot 1 "Input <in-size> bytes",
// slot 2 "Output <out-size> bytes", NameOfStation = --name, IP = the IP of the NIC.
#include "softpn/pn_device.hpp"

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
    softpn::DeviceConfig cfg;
    cfg.stationName = "softpn-device";
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--list") {
            std::string err;
            auto list = softpn::Device::listAdapters(&err);
            if (list.empty())
                std::printf("%s\n", err.c_str());
            for (const auto& a : list)
                std::printf("%s\n", a.c_str());
            return 0;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", k.c_str());
            return 2;
        }
        const char* v = argv[++i];
        unsigned long n = std::strtoul(v, nullptr, 0);
        if (k == "--adapter") cfg.adapter = v;
        else if (k == "--name") cfg.stationName = v;
        else if (k == "--in-size") cfg.inputSize = n;
        else if (k == "--out-size") cfg.outputSize = n;
        else if (k == "--min-update-ms") cfg.minUpdateTimeMs = uint32_t(n);
        else if (k == "--no-vlan") cfg.sendVlanTag = n == 0;
        else if (k == "--promisc") cfg.promiscuous = n != 0;
        else if (k == "--priority") cfg.raiseThreadPriority = n != 0;
        else if (k == "--persist") cfg.persistPath = v;
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }
    if (cfg.adapter.empty()) {
        std::fprintf(stderr, "--adapter is required (--list shows the adapters)\n");
        return 2;
    }

    cfg.onLog = logLine;
    cfg.onStateChanged = [](softpn::DeviceState s) { logLine(std::string("state: ") + softpn::Device::stateName(s)); };
    cfg.onSignal = [] { logLine("*** DCP signal: this is the device you are looking for ***"); };
    cfg.onOutputs = [](const std::vector<uint8_t>& out, bool run) {
        std::string line = std::string("outputs (PLC -> PC, ") + (run ? "RUN" : "STOP") + ")";
        for (size_t i = 0; i < out.size() && i < 16; ++i) {
            char b[4];
            std::snprintf(b, sizeof b, " %02X", out[i]);
            line += b;
        }
        logLine(line + (out.size() > 16 ? " ..." : ""));
    };

    softpn::Device dev(cfg);
    std::string error;
    if (!dev.start(&error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    logLine("running, Ctrl+C to quit");

    std::vector<uint8_t> in(cfg.inputSize, 0);
    uint32_t heartbeat = 0;
    auto nextStats = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!g_quit) {
        const std::vector<uint8_t> out = dev.outputData();
        for (size_t i = 4; i < in.size() && i < out.size(); ++i)
            in[i] = out[i]; // echo
        ++heartbeat;
        for (size_t i = 0; i < 4 && i < in.size(); ++i)
            in[i] = uint8_t(heartbeat >> (8 * i));
        dev.setInputData(in.data(), in.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() >= nextStats) {
            const softpn::DeviceStats s = dev.stats();
            logLine("stats: tx " + std::to_string(s.txFrames) + " rx " + std::to_string(s.rxFrames) + ", max TX period " +
                    std::to_string(s.maxTxPeriodUs) + " us, late " + std::to_string(s.lateCycles) + ", ARs " +
                    std::to_string(s.arConnects) + ", aborts " + std::to_string(s.arAborts) +
                    (s.lastAbort.empty() ? "" : " (last: " + s.lastAbort + ")"));
            nextStats += std::chrono::seconds(10);
        }
    }
    dev.stop();
    logLine("stopped");
    return 0;
}
