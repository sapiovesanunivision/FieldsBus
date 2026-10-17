// pn_probe: Layer-2 diagnostics for the softpn PROFINET device (a mini "Proneta").
//
//   pn_probe --list                                        adapters with MAC and IPv4
//   --backend auto|npcap|ndis (any mode)                   Windows raw-Ethernet backend (default auto)
//   pn_probe --adapter A --jitter [--period-ms 8] [--seconds 60] [--dst MAC]
//            sends 0x88B5 test frames every period and reports the achieved TX period
//            (min/avg/max/p99 and a histogram): Gate A of the PROFINET PoC.
//            --dst: send to that MAC (the --rx-jitter side). Default: an unused locally
//            administered MAC, which physical switches flood but Hyper-V switches drop.
//   pn_probe --adapter A --rx-jitter [--seconds 60]        inter-arrival times of 0x88B5 frames
//            (run on a second PC / NIC while the other side runs --jitter)
//   pn_probe --adapter A --identify [--name n]             DCP Identify (all devices or one name)
//   pn_probe --adapter A --set-name MAC NAME [--temp 1]    DCP Set NameOfStation
//   pn_probe --adapter A --blink MAC                       DCP Set Control/Signal (flash LED)
//
// 0x88B5 is the IEEE "local experimental" EtherType: PROFINET devices ignore those frames.
#include "softeip/socket_compat.hpp" // first (winsock2.h before windows.h); timeBeginPeriod, no throttling
#include "softeip/periodic_timer.hpp"

#include "pn_dcp.hpp"
#include "pn_defs.hpp"
#include "pn_frame.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace softpn;
using Clock = std::chrono::steady_clock;

namespace {

void printStats(const char* title, std::vector<double> us, double nominalUs)
{
    if (us.empty()) {
        std::printf("%s: no samples\n", title);
        return;
    }
    std::sort(us.begin(), us.end());
    double sum = 0;
    for (double v : us)
        sum += v;
    auto pct = [&](double p) { return us[std::min(us.size() - 1, size_t(p * double(us.size())))]; };
    std::printf("%s: %zu periods, nominal %.0f us\n", title, us.size(), nominalUs);
    std::printf("  min %.0f  avg %.0f  p50 %.0f  p99 %.0f  p99.9 %.0f  max %.0f us\n", us.front(),
                sum / double(us.size()), pct(0.5), pct(0.99), pct(0.999), us.back());
    // Histogram of the deviation from nominal.
    const double edges[] = {100, 250, 500, 1000, 2000, 5000};
    size_t counts[7] = {};
    for (double v : us) {
        const double dev = v > nominalUs ? v - nominalUs : nominalUs - v;
        size_t b = 0;
        while (b < 6 && dev >= edges[b])
            ++b;
        ++counts[b];
    }
    const char* labels[] = {"< 0.1 ms", "< 0.25 ms", "< 0.5 ms", "< 1 ms", "< 2 ms", "< 5 ms", ">= 5 ms"};
    for (size_t b = 0; b < 7; ++b)
        std::printf("  |dev| %-10s %8zu  (%.3f %%)\n", labels[b], counts[b], 100.0 * double(counts[b]) / double(us.size()));
}

// Same scheduling as the device's cyclic thread (pn_device.cpp): wait for the next due time with a
// one-shot timer, send, advance the due time by one period. A late wake-up delays one frame but
// does not shift the schedule (no drift, no burst).
int jitter(L2Port& port, unsigned periodMs, int seconds, const MacAddress& dst)
{
    softeip::DeadlineTimer timer;
    std::printf("TX every %u ms for %d s on %s (%s)\n", periodMs, seconds, port.info().friendlyName.c_str(),
                timer.kind());
    std::vector<double> periods, sendUs;
    periods.reserve(size_t(seconds) * 1000 / periodMs + 16);
    sendUs.reserve(periods.capacity());
    const auto period = std::chrono::milliseconds(periodMs);
    uint32_t seq = 0;
    Clock::time_point last{};
    auto next = Clock::now();
    const auto end = next + std::chrono::seconds(seconds);
    while (Clock::now() < end) {
        timer.waitUntil(next);
        const auto now = Clock::now();
        next += period;
        if (next <= now) // fell behind: resync instead of bursting
            next = now + period;
        ByteWriter w;
        writeEthHeader(w, dst, port.info().mac, kEtherTypeLocalExperimental);
        w.u32be(seq++);
        std::vector<uint8_t> f = std::move(w.data());
        padFrame(f);
        port.send(f);
        const auto sent = Clock::now();
        sendUs.push_back(double(std::chrono::duration_cast<std::chrono::microseconds>(sent - now).count()));
        if (last != Clock::time_point{})
            periods.push_back(double(std::chrono::duration_cast<std::chrono::microseconds>(now - last).count()));
        last = now;
    }
    printStats("TX period", periods, periodMs * 1000.0);
    std::sort(sendUs.begin(), sendUs.end());
    if (!sendUs.empty())
        std::printf("  send() duration: p50 %.0f  p99 %.0f  max %.0f us\n", sendUs[sendUs.size() / 2],
                    sendUs[std::min(sendUs.size() - 1, size_t(0.99 * double(sendUs.size())))], sendUs.back());
    return 0;
}

int rxJitter(L2Port& port, int seconds)
{
    std::printf("receiving 0x88B5 frames for %d s on %s\n", seconds, port.info().friendlyName.c_str());
    std::vector<double> gaps;
    std::vector<uint8_t> f;
    Clock::time_point last{};
    uint32_t lastSeq = 0, lost = 0;
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < end) {
        if (!port.receive(f, 100))
            continue;
        EthHeader eth;
        if (!parseEthHeader(f.data(), f.size(), eth) || eth.etherType != kEtherTypeLocalExperimental ||
            f.size() < eth.payloadOffset + 4)
            continue;
        const auto now = Clock::now();
        const uint32_t seq = (uint32_t(f[eth.payloadOffset]) << 24) | (uint32_t(f[eth.payloadOffset + 1]) << 16) |
                             (uint32_t(f[eth.payloadOffset + 2]) << 8) | f[eth.payloadOffset + 3];
        if (last != Clock::time_point{}) {
            gaps.push_back(double(std::chrono::duration_cast<std::chrono::microseconds>(now - last).count()));
            if (seq > lastSeq + 1)
                lost += seq - lastSeq - 1;
        }
        last = now;
        lastSeq = seq;
    }
    double nominal = 0;
    if (!gaps.empty()) {
        std::vector<double> s = gaps;
        std::sort(s.begin(), s.end());
        nominal = s[s.size() / 2];
    }
    printStats("RX inter-arrival", gaps, nominal);
    std::printf("  lost frames: %u\n", lost);
    return 0;
}

struct Answer {
    MacAddress mac;
    std::string name, vendor;
    uint32_t ip = 0;
    uint16_t vendorId = 0, deviceId = 0;
    uint8_t setError = 0xFF;
};

std::vector<Answer> dcpTransaction(L2Port& port, const MacAddress& dst, DcpPdu req, int waitMs)
{
    req.xid = uint32_t(Clock::now().time_since_epoch().count());
    port.send(buildDcpFrame(dst, port.info().mac, req));
    std::vector<Answer> answers;
    std::vector<uint8_t> f;
    const auto end = Clock::now() + std::chrono::milliseconds(waitMs);
    while (Clock::now() < end) {
        if (!port.receive(f, 50))
            continue;
        EthHeader eth;
        if (!parseEthHeader(f.data(), f.size(), eth) || eth.etherType != kEtherTypeProfinet)
            continue;
        DcpPdu res;
        try {
            res = parseDcp(f.data() + eth.payloadOffset, f.size() - eth.payloadOffset);
        } catch (const ParseError&) {
            continue;
        }
        if (res.xid != req.xid || res.serviceType == dcp::kTypeRequest)
            continue;
        Answer a;
        a.mac = eth.src;
        for (const DcpBlock& b : res.blocks) {
            const auto p = dcpPayload(b);
            if (b.option == dcp::kOptDeviceProperties && b.suboption == dcp::kSubDevNameOfStation)
                a.name.assign(p.begin(), p.end());
            else if (b.option == dcp::kOptDeviceProperties && b.suboption == dcp::kSubDevVendor)
                a.vendor.assign(p.begin(), p.end());
            else if (b.option == dcp::kOptDeviceProperties && b.suboption == dcp::kSubDevId && p.size() >= 4) {
                a.vendorId = uint16_t((p[0] << 8) | p[1]);
                a.deviceId = uint16_t((p[2] << 8) | p[3]);
            } else if (b.option == dcp::kOptIp && b.suboption == dcp::kSubIpParameter && p.size() >= 4)
                a.ip = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
            else if (b.option == dcp::kOptControl && b.suboption == dcp::kSubCtrlResponse && b.data.size() >= 3)
                a.setError = b.data[2];
        }
        answers.push_back(a);
        if (req.serviceId != dcp::kServiceIdentify)
            break; // unicast request: one answer
    }
    return answers;
}

} // namespace

int main(int argc, char** argv)
{
    softeip::SocketLibrary sockets; // 1 ms timer resolution, no power throttling
    std::string adapter, name, mode;
    std::string arg1, arg2;
    unsigned periodMs = 8;
    std::string dstText = "02:00:5e:00:88:b5"; // locally administered, nobody owns it
    int seconds = 60;
    bool temporary = false;
    L2Backend backend = L2Backend::Auto;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (k == "--list" || k == "--jitter" || k == "--rx-jitter" || k == "--identify")
            mode = k;
        else if (k == "--set-name") {
            mode = k;
            arg1 = next();
            arg2 = next();
        } else if (k == "--blink") {
            mode = k;
            arg1 = next();
        } else if (k == "--adapter")
            adapter = next();
        else if (k == "--backend") {
            if (!parseL2Backend(next(), backend)) {
                std::fprintf(stderr, "--backend: auto, npcap or ndis\n");
                return 2;
            }
        } else if (k == "--name")
            name = next();
        else if (k == "--dst")
            dstText = next();
        else if (k == "--period-ms")
            periodMs = unsigned(std::strtoul(next().c_str(), nullptr, 0));
        else if (k == "--seconds")
            seconds = int(std::strtol(next().c_str(), nullptr, 0));
        else if (k == "--temp")
            temporary = std::strtoul(next().c_str(), nullptr, 0) != 0;
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }

    std::string err;
    if (mode == "--list" || mode.empty()) {
        std::vector<AdapterInfo> list = listAdapters(backend, &err);
        if (list.empty())
            std::printf("%s\n", err.empty() ? "no adapters" : err.c_str());
        for (const AdapterInfo& a : list)
            std::printf("%-22s %s  %-15s  %s\n    %s  [%s]\n", a.friendlyName.c_str(), a.mac.str().c_str(),
                        a.ipv4 ? ipToString(a.ipv4).c_str() : "no IPv4", a.description.c_str(), a.name.c_str(),
                        a.backend.c_str());
        return list.empty() ? 1 : 0;
    }
    if (adapter.empty()) {
        std::fprintf(stderr, "--adapter is required (see --list)\n");
        return 2;
    }
    L2Options opt;
    opt.extraEtherType = kEtherTypeLocalExperimental;
    opt.backend = backend;
    std::unique_ptr<L2Port> port = openL2Port(adapter, opt, &err);
    if (!port) {
        std::printf("%s\n", err.c_str());
        return 1;
    }

    if (mode == "--jitter") {
        MacAddress dst;
        if (!MacAddress::parse(dstText, dst)) {
            std::fprintf(stderr, "invalid --dst MAC '%s'\n", dstText.c_str());
            return 2;
        }
        return jitter(*port, std::max(1u, periodMs), seconds, dst);
    }
    if (mode == "--rx-jitter")
        return rxJitter(*port, seconds);

    DcpPdu req;
    req.serviceType = dcp::kTypeRequest;
    if (mode == "--identify") {
        req.frameId = kFrameIdDcpIdentifyReq;
        req.serviceId = dcp::kServiceIdentify;
        req.responseDelay = 128; // devices spread their answers over 1.28 s
        DcpBlock filter;
        if (name.empty()) {
            filter.option = dcp::kOptAllSelector;
            filter.suboption = dcp::kSubAllSelector;
        } else {
            filter.option = dcp::kOptDeviceProperties;
            filter.suboption = dcp::kSubDevNameOfStation;
            filter.data.assign(name.begin(), name.end());
        }
        req.blocks.push_back(filter);
        auto answers = dcpTransaction(*port, MacAddress::fromBytes(dcp::kMulticastIdentify), req, 1500);
        for (const Answer& a : answers)
            std::printf("%s  %-15s  name '%s'  vendor 0x%04X device 0x%04X  '%s'\n", a.mac.str().c_str(),
                        ipToString(a.ip).c_str(), a.name.c_str(), a.vendorId, a.deviceId, a.vendor.c_str());
        std::printf("%zu device(s)\n", answers.size());
        return 0;
    }

    MacAddress mac;
    if (!MacAddress::parse(arg1, mac)) {
        std::fprintf(stderr, "invalid MAC address '%s'\n", arg1.c_str());
        return 2;
    }
    req.frameId = kFrameIdDcpGetSet;
    req.serviceId = dcp::kServiceSet;
    if (mode == "--set-name")
        req.blocks.push_back(makeDcpBlock(dcp::kOptDeviceProperties, dcp::kSubDevNameOfStation,
                                          temporary ? dcp::kQualifierTemporary : dcp::kQualifierPermanent, arg2));
    else { // --blink
        const uint8_t signal[2] = {uint8_t(dcp::kSignalFlashOnce >> 8), uint8_t(dcp::kSignalFlashOnce & 0xFF)};
        req.blocks.push_back(makeDcpBlock(dcp::kOptControl, dcp::kSubCtrlSignal, 0, signal, 2));
    }
    auto answers = dcpTransaction(*port, mac, req, 1000);
    if (answers.empty()) {
        std::printf("no answer from %s\n", mac.str().c_str());
        return 1;
    }
    std::printf("%s: BlockError %u (%s)\n", mac.str().c_str(), answers[0].setError,
                answers[0].setError == 0 ? "OK" : "refused");
    return answers[0].setError == 0 ? 0 : 1;
}
