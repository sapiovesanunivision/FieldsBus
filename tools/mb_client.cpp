// mb_client: manual Modbus CLIENT (formerly "master") for any Modbus SERVER (formerly "slave"):
// a PLC, a Wago coupler, EasyModbus Server Simulator, mb_server_demo, ...
//
// Usage: mb_client [--host IP] [--port N] [--transport tcp|udp] [--unit N] [--timeout-ms N] [--retries N]
//                  <command> [args]
//   read-holding  ADDR COUNT        FC03     read-input    ADDR COUNT   FC04
//   read-coils    ADDR COUNT        FC01     read-discrete ADDR COUNT   FC02
//   write-register  ADDR VALUE      FC06     write-registers ADDR V1 V2 ...   FC16
//   write-coil      ADDR 0|1        FC05     write-coils     ADDR B1 B2 ...   FC15
//   poll [--read TABLE:ADDR:COUNT]... [--write TABLE:ADDR:COUNT]... [--cycle-ms N] [--seconds N]
//        TABLE = holding | input | coils | discrete. Every write area gets a counter that
//        increments once per second; changes of the read areas are printed.
// Addresses are 0-based protocol addresses (EasyModbus' UI shows address + 1).
// Values accept decimal or 0x hex. Exit code 0 = ok, 1 = Modbus/communication error, 2 = usage.
#include "softmb/modbus_client.hpp"
#include "softmb/modbus_client_poller.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace softmb;

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

int usage()
{
    std::fprintf(stderr,
                 "usage: mb_client [--host IP] [--port N] [--transport tcp|udp] [--unit N] [--timeout-ms N] [--retries N]\n"
                 "                 read-holding|read-input|read-coils|read-discrete ADDR COUNT\n"
                 "               | write-register ADDR VALUE | write-registers ADDR V1 V2 ...\n"
                 "               | write-coil ADDR 0|1 | write-coils ADDR B1 B2 ...\n"
                 "               | poll [--read TABLE:ADDR:COUNT]... [--write TABLE:ADDR:COUNT]... [--cycle-ms N] [--seconds N]\n"
                 "  TABLE = holding | input | coils | discrete. Addresses are 0-based (EasyModbus shows +1).\n");
    return 2;
}

bool parseNum(const char* s, unsigned long& v)
{
    char* end = nullptr;
    v = std::strtoul(s, &end, 0);
    return end && *end == '\0' && end != s;
}

bool parseArea(const std::string& spec, PollArea& a)
{
    size_t p1 = spec.find(':');
    size_t p2 = spec.find(':', p1 == std::string::npos ? p1 : p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos)
        return false;
    std::string t = spec.substr(0, p1);
    if (t == "holding" || t == "hr") a.table = Table::HoldingRegisters;
    else if (t == "input" || t == "ir") a.table = Table::InputRegisters;
    else if (t == "coils" || t == "co") a.table = Table::Coils;
    else if (t == "discrete" || t == "di") a.table = Table::DiscreteInputs;
    else return false;
    unsigned long addr = 0, count = 0;
    if (!parseNum(spec.substr(p1 + 1, p2 - p1 - 1).c_str(), addr) || !parseNum(spec.substr(p2 + 1).c_str(), count) ||
        addr > 65535 || count == 0 || count > 65536)
        return false;
    a.address = uint16_t(addr);
    a.count = uint16_t(count);
    return true;
}

bool isBits(Table t) { return t == Table::Coils || t == Table::DiscreteInputs; }
size_t areaBytes(const PollArea& a) { return isBits(a.table) ? (size_t(a.count) + 7) / 8 : size_t(a.count) * 2; }

const char* tableName(Table t)
{
    switch (t) {
    case Table::Coils: return "coils";
    case Table::DiscreteInputs: return "discrete";
    case Table::HoldingRegisters: return "holding";
    case Table::InputRegisters: return "input";
    }
    return "?";
}

void printRegs(uint16_t addr, const std::vector<uint16_t>& v)
{
    for (size_t i = 0; i < v.size(); ++i)
        std::printf("  %5u: %6u  0x%04X\n", unsigned(addr + i), v[i], v[i]);
}

void printBits(uint16_t addr, const std::vector<bool>& v)
{
    for (size_t i = 0; i < v.size(); i += 16) {
        std::printf("  %5u:", unsigned(addr + i));
        for (size_t j = i; j < v.size() && j < i + 16; ++j)
            std::printf(" %d", v[j] ? 1 : 0);
        std::printf("\n");
    }
}

int report(const Result& r, const char* what)
{
    if (r.ok())
        return 0;
    std::printf("%s failed: %s\n", what, r.text().c_str());
    return 1;
}

int runPoll(const ModbusClientConfig& cc, int argc, char** argv, int i)
{
    ModbusClientPollerConfig pc;
    pc.client = cc;
    unsigned long seconds = 0; // 0 = until Ctrl+C
    for (; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        PollArea a;
        unsigned long n = 0;
        if (k == "--read" && parseArea(argv[i + 1], a)) {
            a.imageOffset = pc.inputSize;
            pc.inputSize += areaBytes(a);
            pc.reads.push_back(a);
        } else if (k == "--write" && parseArea(argv[i + 1], a)) {
            a.imageOffset = pc.outputSize;
            pc.outputSize += areaBytes(a);
            pc.writes.push_back(a);
        } else if (k == "--cycle-ms" && parseNum(argv[i + 1], n) && n > 0) {
            pc.cycleMs = unsigned(n);
        } else if (k == "--seconds" && parseNum(argv[i + 1], n)) {
            seconds = n;
        } else {
            return usage();
        }
    }
    if (i != argc)
        return usage();

    const auto t0 = std::chrono::steady_clock::now();
    auto stamp = [t0]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    pc.onLog = [&](const std::string& m) { std::printf("[%8.3f] %s\n", stamp(), m.c_str()); };
    const std::vector<PollArea> reads = pc.reads;
    pc.onInputsChanged = [&, reads](const std::vector<uint8_t>& in) {
        for (const auto& a : reads) {
            std::printf("[%8.3f] %s %u..%u:", stamp(), tableName(a.table), unsigned(a.address),
                        unsigned(a.address + a.count - 1));
            const uint8_t* p = in.data() + a.imageOffset;
            if (isBits(a.table)) {
                for (size_t b = 0; b < a.count && b < 64; ++b)
                    std::printf("%s%d", b % 8 ? "" : " ", (p[b / 8] >> (b % 8)) & 1);
            } else {
                for (size_t r = 0; r < a.count && r < 16; ++r)
                    std::printf(" %u", unsigned((p[r * 2] << 8) | p[r * 2 + 1]));
            }
            std::printf("%s\n", (isBits(a.table) ? a.count > 64 : a.count > 16) ? " ..." : "");
        }
    };

    ModbusClientPoller poller(pc);
    std::string error;
    if (!poller.start(&error)) {
        std::fprintf(stderr, "poll: %s\n", error.c_str());
        return 2;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    uint16_t counter = 0;
    auto nextTick = std::chrono::steady_clock::now();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_quit && (seconds == 0 || std::chrono::steady_clock::now() < end)) {
        if (std::chrono::steady_clock::now() >= nextTick && !pc.writes.empty()) {
            ++counter; // every write area: registers = counter, coils = counter bit pattern
            std::vector<uint8_t> out(pc.outputSize);
            for (size_t b = 0; b + 1 < out.size(); b += 2) {
                out[b] = uint8_t(counter >> 8);
                out[b + 1] = uint8_t(counter);
            }
            if (out.size() % 2)
                out.back() = uint8_t(counter);
            poller.ioWrite(0, out.data(), out.size());
            nextTick += std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto st = poller.stats();
    poller.stop();
    std::printf("poll: %llu cycles, %llu failed, last cycle %.2f ms, max cycle %.2f ms, max period %.2f ms%s%s\n",
                static_cast<unsigned long long>(st.cycles), static_cast<unsigned long long>(st.failedCycles),
                st.lastCycleMs, st.maxCycleMs, st.maxPeriodMs, st.lastError.empty() ? "" : ", last error: ",
                st.lastError.c_str());
    return st.failedCycles ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    ModbusClientConfig cc;
    int i = 1;
    for (; i + 1 < argc && std::strncmp(argv[i], "--", 2) == 0; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        unsigned long n = 0;
        if (k == "--host") cc.host = v;
        else if (k == "--port" && parseNum(v, n)) cc.port = uint16_t(n);
        else if (k == "--transport") cc.transport = std::string(v) == "udp" ? ClientTransport::Udp : ClientTransport::Tcp;
        else if (k == "--unit" && parseNum(v, n)) cc.unitId = uint8_t(n);
        else if (k == "--timeout-ms" && parseNum(v, n)) cc.responseTimeoutMs = unsigned(n);
        else if (k == "--retries" && parseNum(v, n)) cc.retries = unsigned(n);
        else return usage();
    }
    if (i >= argc)
        return usage();
    const std::string cmd = argv[i++];
    if (cmd == "poll")
        return runPoll(cc, argc, argv, i);

    std::vector<unsigned long> nums;
    for (; i < argc; ++i) {
        unsigned long n = 0;
        if (!parseNum(argv[i], n))
            return usage();
        nums.push_back(n);
    }
    if (nums.size() < 2 || nums[0] > 65535)
        return usage();
    const uint16_t addr = uint16_t(nums[0]);

    ModbusClient c(cc);
    std::printf("Modbus %s client -> server %s:%u unit %u\n", cc.transport == ClientTransport::Udp ? "UDP" : "TCP",
                cc.host.c_str(), cc.port, cc.unitId);
    if (cmd == "read-holding" || cmd == "read-input") {
        std::vector<uint16_t> v;
        Result r = cmd == "read-holding" ? c.readHoldingRegisters(addr, uint16_t(nums[1]), v)
                                         : c.readInputRegisters(addr, uint16_t(nums[1]), v);
        if (r.ok())
            printRegs(addr, v);
        return report(r, cmd.c_str());
    }
    if (cmd == "read-coils" || cmd == "read-discrete") {
        std::vector<bool> v;
        Result r = cmd == "read-coils" ? c.readCoils(addr, uint16_t(nums[1]), v)
                                       : c.readDiscreteInputs(addr, uint16_t(nums[1]), v);
        if (r.ok())
            printBits(addr, v);
        return report(r, cmd.c_str());
    }
    if (cmd == "write-register") {
        Result r = c.writeSingleRegister(addr, uint16_t(nums[1]));
        if (r.ok())
            std::printf("  register %u = %lu\n", unsigned(addr), nums[1]);
        return report(r, cmd.c_str());
    }
    if (cmd == "write-registers") {
        std::vector<uint16_t> v;
        for (size_t k = 1; k < nums.size(); ++k)
            v.push_back(uint16_t(nums[k]));
        Result r = c.writeMultipleRegisters(addr, v);
        if (r.ok())
            std::printf("  wrote %zu registers at %u\n", v.size(), unsigned(addr));
        return report(r, cmd.c_str());
    }
    if (cmd == "write-coil") {
        Result r = c.writeSingleCoil(addr, nums[1] != 0);
        if (r.ok())
            std::printf("  coil %u = %d\n", unsigned(addr), nums[1] != 0 ? 1 : 0);
        return report(r, cmd.c_str());
    }
    if (cmd == "write-coils") {
        std::vector<bool> v;
        for (size_t k = 1; k < nums.size(); ++k)
            v.push_back(nums[k] != 0);
        Result r = c.writeMultipleCoils(addr, v);
        if (r.ok())
            std::printf("  wrote %zu coils at %u\n", v.size(), unsigned(addr));
        return report(r, cmd.c_str());
    }
    return usage();
}
