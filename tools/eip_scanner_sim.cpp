// Minimal EtherNet/IP *scanner* (PLC side) used to test the adapter without a real PLC, built on
// softeip::Scanner. It connects like a PLC, exchanges cyclic class-1 I/O for a while and checks the echo of
// eip_adapter_demo.
//
// Usage: eip_scanner_sim [--target IP] [--rpi-ms N] [--seconds N] [--local-port N]
//                        [--in-size N] [--out-size N] [--o2t N] [--t2o N] [--cfg N]
//                        [--skip-close 1]
//
// --local-port: UDP port we receive T->O on (announced with a Sockaddr Info item). Use something other than
// 2222 when the adapter runs on the same PC.
#include "softeip/eip_scanner.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace softeip;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv)
{
    std::string target = "127.0.0.1";
    uint32_t rpiMs = 10;
    int seconds = 5;
    uint16_t localPort = 2223;
    uint16_t inSize = 32, outSize = 32;
    uint16_t o2t = 150, t2o = 100, cfgInst = 151;
    bool skipClose = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        unsigned long v = std::strtoul(argv[i + 1], nullptr, 0);
        if (k == "--target") target = argv[i + 1];
        else if (k == "--rpi-ms") rpiMs = uint32_t(v);
        else if (k == "--seconds") seconds = int(v);
        else if (k == "--local-port") localPort = uint16_t(v);
        else if (k == "--in-size") inSize = uint16_t(v);
        else if (k == "--out-size") outSize = uint16_t(v);
        else if (k == "--o2t") o2t = uint16_t(v);
        else if (k == "--t2o") t2o = uint16_t(v);
        else if (k == "--cfg") cfgInst = uint16_t(v);
        else if (k == "--skip-close") skipClose = v != 0;
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }

    std::mutex outMutex;
    std::vector<uint8_t> outputs(outSize, 0);
    std::atomic<uint32_t> echoMatches{0};
    std::atomic<uint32_t> lastHeartbeat{0};

    ScannerConfig sc;
    sc.target = target;
    sc.localIoPort = localPort;
    sc.rpiUs = rpiMs * 1000;
    sc.inputSize = inSize;
    sc.outputSize = outSize;
    sc.o2tInstance = o2t;
    sc.t2oInstance = t2o;
    sc.configInstance = cfgInst;
    sc.reconnectDelayMs = 60000; // one connection per run: a failure ends the test
    sc.onLog = [](const std::string& m) { std::printf("%s\n", m.c_str()); };
    sc.onInputs = [&](const uint8_t* in, size_t n) {
        if (n >= 4)
            lastHeartbeat = uint32_t(in[0] | (in[1] << 8) | (in[2] << 16) | (uint32_t(in[3]) << 24));
        std::lock_guard<std::mutex> lock(outMutex);
        if (n > 4 && outputs.size() >= n && std::memcmp(in + 4, outputs.data() + 4, n - 4) == 0)
            ++echoMatches;
    };
    Scanner scanner(sc);
    std::string error;
    if (!scanner.start(&error)) {
        std::printf("%s\n", error.c_str());
        return 2;
    }

    // Wait for the connection (or its failure).
    const auto connectDeadline = Clock::now() + std::chrono::seconds(5);
    while (!scanner.connected() && Clock::now() < connectDeadline) {
        auto st = scanner.stats();
        if (st.lastOpenStatus != 0) {
            scanner.stop(false);
            return 3; // Forward_Open rejected (logged)
        }
        if (!st.lastError.empty() && st.connects == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!scanner.connected()) {
        std::printf("no connection: %s\n", scanner.stats().lastError.c_str());
        scanner.stop(false);
        return 1;
    }

    const auto start = Clock::now();
    const auto end = start + std::chrono::seconds(seconds);
    auto nextPrint = start + std::chrono::seconds(1);
    while (Clock::now() < end) {
        // Output pattern changes every 100 ms so the echo is observable.
        const uint32_t tick =
            uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() / 100);
        {
            std::lock_guard<std::mutex> lock(outMutex);
            for (size_t i = 0; i < outputs.size(); ++i)
                outputs[i] = uint8_t(tick + i);
            scanner.ioWrite(0, outputs.data(), outputs.size());
        }
        if (Clock::now() >= nextPrint) {
            std::printf("  T->O packets %llu, echo matches %u, adapter heartbeat %u\n",
                        static_cast<unsigned long long>(scanner.stats().received), echoMatches.load(),
                        lastHeartbeat.load());
            nextPrint += std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const uint64_t received = scanner.stats().received;

    if (skipClose)
        std::printf("Forward_Close skipped: adapter watchdog should time the connection out\n");
    scanner.stop(!skipClose);
    const bool closeOk = skipClose || scanner.stats().forwardCloseOk;

    const uint32_t expected = uint32_t(seconds * 1000 / rpiMs);
    // Echo is only checkable when the adapter can echo our outputs (not for input-only / listen-only).
    const bool echoExpected = inSize > 4 && outSize >= inSize;
    const bool pass = closeOk && received > expected / 2 && (!echoExpected || echoMatches > 0);
    std::printf("RESULT: %s (T->O %llu of ~%u expected, echo matches %u)\n", pass ? "PASS" : "FAIL",
                static_cast<unsigned long long>(received), expected, echoMatches.load());
    return pass ? 0 : 1;
}
