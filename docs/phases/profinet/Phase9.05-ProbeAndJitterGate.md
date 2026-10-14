# Phase 9.05: pn_probe and the timing gate (Gate A)

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
A diagnostic tool for Layer 2: list adapters, measure how regularly Windows can send frames (the decisive question for a software PROFINET device), measure arrival jitter on a second machine, and run DCP Identify / Set name / Blink like a mini Proneta.

## Documentation References
- [CreateWaitableTimerExW](https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
- [timeBeginPeriod and power throttling](https://learn.microsoft.com/windows/win32/api/timeapi/nf-timeapi-timebeginperiod)

---

## Step 9.05.1: DeadlineTimer: wake up at an absolute time

**File:** `include/softeip/periodic_timer.hpp`

### Purpose
Adds `DeadlineTimer` next to the existing `PeriodicTimer`. Measurements on this PC showed that a periodic 1 ms waitable timer actually ticks about every 1.5 ms; a one-shot high-resolution timer set to the exact due time wakes much closer to it.

### Implementation

```cpp
// Fixed-period cycle timer (internal helper; include it from .cpp files, not from public API headers).
//  Windows: a periodic waitable timer. Prefers CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
//           (Windows 10 1803+: sub-ms accuracy without timeBeginPeriod, unaffected by
//           Windows 11 timer-resolution throttling); falls back to a normal waitable timer.
//  Other:   steady_clock + sleep_until (resync instead of bursting when late).
#pragma once

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// synchapi.h only defines it for _WIN32_WINNT >= Windows 10 RS4; the project targets 0x0601.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace softeip {

class PeriodicTimer {
public:
    explicit PeriodicTimer(std::chrono::milliseconds period)
        : period_(period), next_(std::chrono::steady_clock::now())
    {
#ifdef _WIN32
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        highResolution_ = timer_ != nullptr;
        if (!timer_) // older Windows rejects the flag
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(period.count()) * 10000; // relative, 100 ns units
            if (!SetWaitableTimer(timer_, &due, static_cast<LONG>(period.count()), nullptr, nullptr, FALSE)) {
                CloseHandle(timer_);
                timer_ = nullptr;
            }
        }
#endif
    }
    ~PeriodicTimer()
    {
#ifdef _WIN32
        if (timer_)
            CloseHandle(timer_);
#endif
    }
    PeriodicTimer(const PeriodicTimer&) = delete;
    PeriodicTimer& operator=(const PeriodicTimer&) = delete;

    // Blocks until the next period boundary. A missed period is not caught up (no burst).
    void wait()
    {
#ifdef _WIN32
        if (timer_) {
            WaitForSingleObject(timer_, INFINITE);
            return;
        }
#endif
        next_ += period_;
        const auto now = std::chrono::steady_clock::now();
        if (next_ <= now)
            next_ = now + period_;
        std::this_thread::sleep_until(next_);
    }

    const char* kind() const
    {
#ifdef _WIN32
        if (timer_)
            return highResolution_ ? "high-resolution waitable timer" : "waitable timer";
#endif
        return "steady_clock sleep_until";
    }

private:
    std::chrono::milliseconds period_;
    std::chrono::steady_clock::time_point next_;
#ifdef _WIN32
    HANDLE timer_ = nullptr;
    bool highResolution_ = false;
#endif
};

// Waits until an absolute steady_clock time (one-shot). On Windows a high-resolution one-shot
// waitable timer wakes much closer to the deadline than the ticks of a periodic 1 ms timer,
// which fire every ~1.5 ms on some PCs.
class DeadlineTimer {
public:
    DeadlineTimer()
    {
#ifdef _WIN32
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        highResolution_ = timer_ != nullptr;
        if (!timer_)
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
#endif
    }
    ~DeadlineTimer()
    {
#ifdef _WIN32
        if (timer_)
            CloseHandle(timer_);
#endif
    }
    DeadlineTimer(const DeadlineTimer&) = delete;
    DeadlineTimer& operator=(const DeadlineTimer&) = delete;

    void waitUntil(std::chrono::steady_clock::time_point deadline)
    {
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now)
            return;
#ifdef _WIN32
        if (timer_) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(ns / 100); // relative, 100 ns units
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer_, INFINITE);
                return;
            }
        }
#endif
        std::this_thread::sleep_until(deadline);
    }

    const char* kind() const
    {
#ifdef _WIN32
        if (timer_)
            return highResolution_ ? "high-resolution one-shot timer" : "one-shot waitable timer";
#endif
        return "steady_clock sleep_until";
    }

private:
#ifdef _WIN32
    HANDLE timer_ = nullptr;
    bool highResolution_ = false;
#endif
};

} // namespace softeip
```

### Key Concepts
1. **`waitUntil(steady_clock::time_point)`**: the cyclic thread computes absolute due times (`next += period`), so a late wake-up delays one frame but never shifts the schedule.
2. **Additive**: `PeriodicTimer` (used by EtherNet/IP) is unchanged.

---

## Step 9.05.2: `pn_probe.cpp`

**File:** `tools/pn_probe.cpp`

### Purpose
The tool. `--jitter` uses exactly the device's scheduling (Phase 9.13), so its numbers predict the device's TX timing. 0x88B5 (IEEE local experimental) test frames are ignored by PROFINET devices.

### Implementation

```cpp
// pn_probe: Layer-2 diagnostics for the softpn PROFINET device (a mini "Proneta").
//
//   pn_probe --list                                        adapters with MAC and IPv4
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
        else if (k == "--name")
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
        std::vector<AdapterInfo> list = listAdapters(&err);
        if (list.empty())
            std::printf("%s\n", err.empty() ? "no adapters" : err.c_str());
        for (const AdapterInfo& a : list)
            std::printf("%-22s %s  %-15s  %s\n    %s\n", a.friendlyName.c_str(), a.mac.str().c_str(),
                        a.ipv4 ? ipToString(a.ipv4).c_str() : "no IPv4", a.description.c_str(), a.name.c_str());
        return list.empty() ? 1 : 0;
    }
    if (adapter.empty()) {
        std::fprintf(stderr, "--adapter is required (see --list)\n");
        return 2;
    }
    L2Options opt;
    opt.extraEtherType = kEtherTypeLocalExperimental;
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
```

### Key Concepts
1. **`--dst MAC`**: physical switches flood frames to an unknown MAC, Hyper-V switches drop them - send to the receiver's MAC when measuring through a virtual switch.
2. **`send()` duration**: how long `pcap_sendpacket` blocks; outliers here add to the jitter.
3. **Histogram of |deviation|**: what matters is the tail, because the PLC aborts after DataHoldFactor missed periods (typically 3 x update time).

---

## Step 9.05.3: Build pn_probe

**File:** `CMakeLists.txt`

### Purpose
Inside `if(SOFTPN_ENABLED)`, after the library:

### Implementation

```cmake
add_executable(pn_probe tools/pn_probe.cpp)
target_include_directories(pn_probe PRIVATE src/pn)
target_link_libraries(pn_probe PRIVATE softpn)
softeip_warnings(pn_probe)
set_target_properties(pn_probe PROPERTIES FOLDER "tools" VS_DEBUGGER_COMMAND_ARGUMENTS "--list")
```

### Key Concepts
1. **`src/pn` include path**: the probe uses internal headers (DCP codec, frame helpers).

---

## Step 9.05.4: Run Gate A

### Purpose
Decide early whether the PC can hold the update time before investing in the rest.

### Procedure
```bat
pn_probe --list
pn_probe --adapter "Ethernet 2" --jitter --period-ms 8 --seconds 60
pn_probe --adapter "Ethernet 2" --jitter --period-ms 4 --seconds 60
```
Run it on the **dedicated machine NIC** with the PC under its normal load (vision application running).
Optional: on a second PC cabled to it, `pn_probe --adapter X --rx-jitter --seconds 60` while the first
runs `--jitter --dst <MAC of X>`, to see the jitter as the PLC will see it.

**Gate A passes when** at 8 ms: p99 period error < 0.5 ms and max < 2 ms, and no gap reaches
3 x 8 = 24 ms (the PLC's default watchdog).

---

## Measured results

Reference measurement (development laptop, Intel I219-LM idle; test frames on the internal Hyper-V
"Default Switch" adapter, so no LAN traffic; desktop with VS Code running):

| Scheduling | Period | avg | p99 | max | within 0.5 ms |
|---|---|---|---|---|---|
| periodic timer, 1 frame per tick | 8 ms | 8.155 ms (drifts 2 %) | 8.55 ms | 8.78 ms | 94 % |
| periodic timer, 1 frame per tick | 1 ms | 1.47 ms (only ~680 frames/s) | 1.89 ms | 8.6 ms | 26 % |
| **DeadlineTimer (device scheduling)** | 8 ms | 8.000 ms | 8.61 ms | 11.5 ms | 97 % |
| **DeadlineTimer** | 4 ms | 4.000 ms | 4.47 ms | 5.6 ms | 98 % |
| **DeadlineTimer** | 1 ms | 1.005 ms (19 900 / 20 000 frames) | 1.58 ms | 2.6 ms | 98 % |

`send()` (pcap_sendpacket): p50 ~150 us, p99 ~400-550 us, rare outliers of several ms on the virtual adapter.
Repeat on the real station NIC - this table is a reference, not the gate result.

---

## Verification Checklist

- [ ] `pn_probe --list` lists the adapters with MAC / IPv4 / Npcap name
- [ ] Gate A numbers recorded in `docs/phases/phase-9-profinet.md` for the target PC and NIC
- [ ] `pn_probe --identify` (after Phase 9.13) finds the softpn device and any other PROFINET device on the wire

---

## Next Phase
Continue to [Phase9.06-Lldp.md](Phase9.06-Lldp.md): LLDP transmitter.
