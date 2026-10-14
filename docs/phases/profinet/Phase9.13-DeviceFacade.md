# Phase 9.13: softpn::Device and pn_device_demo (Gate B)

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
The public API: one class that opens the NIC, wires DCP, LLDP, RPC, AR, cyclic and RTA, owns the three threads, and offers `setInputData()` / `outputData()` like `softeip::Adapter`. Plus the demo device.

## Documentation References
- [softeip::Adapter usage](../../../README.md#using-the-library)

---

## Step 9.13.1: `pn_device.hpp`

**File:** `include/softpn/pn_device.hpp`

### Purpose
Public header (no winsock/pcap includes, pimpl). Names follow the PLC's view as in softeip: `setInputData` = PC -> PLC (slot 1), `outputData` = PLC -> PC (slot 2).

### Implementation

```cpp
// Software PROFINET RT IO-Device (Conformance Class A, RT_CLASS_1) on a standard PC NIC.
//
// The PLC is the IO-Controller: it finds the device by its NameOfStation (DCP), connects
// (RPC Connect / PrmEnd / ApplicationReady) and then exchanges cyclic I/O every 8..512 ms.
//
//   softpn::DeviceConfig cfg;
//   cfg.adapter = "Ethernet 2";            // or the NIC's MAC / IPv4 / Npcap name
//   cfg.stationName = "vision-station-1";
//   cfg.inputSize = 32;                    // PC -> PLC   (GSDML slot 1, "Input 32 bytes")
//   cfg.outputSize = 32;                   // PLC -> PC   (GSDML slot 2, "Output 32 bytes")
//   softpn::Device dev(cfg);
//   dev.start();
//   dev.setInputData(in, 32);
//   auto out = dev.outputData();
//
// Names follow the PLC's view, as in softeip: "input" = data to the PLC, "output" = from the PLC.
// Windows needs Npcap (https://npcap.com); Linux needs CAP_NET_RAW.
#pragma once

#include "softeip/export.hpp"
#include "softpn/l2_port.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace softpn {

enum class DeviceState {
    Stopped,
    WaitingForController, // started, no AR
    Connecting,           // AR established, parameterisation (Connect .. ApplicationReady)
    DataRun,              // cyclic exchange, controller in RUN
    DataStop,             // cyclic exchange, controller in STOP / outputs not valid
};

struct DeviceConfig {
    std::string adapter;               // NIC: Npcap name, friendly name ("Ethernet 2"), MAC or IPv4
    std::string stationName;           // default NameOfStation when none is persisted ("" = unnamed)
    std::string persistPath = "softpn.ini"; // NameOfStation set by the controller survives restarts
    uint16_t vendorId = 0xFFFF;        // PLACEHOLDER until Univision has a PI vendor ID; must match the GSDML
    uint16_t deviceId = 0x0001;        // must match the GSDML
    std::string orderId = "SOFTPN-1";              // I&M0, max 20 characters
    std::string serialNumber = "0000000000000001"; // I&M0, max 16 characters
    uint16_t hardwareRevision = 1;
    uint8_t softwareRevision[3] = {0, 1, 0};       // I&M0 "V0.1.0"
    std::string deviceVendorValue = "Univision SoftPN"; // DCP TypeOfStation
    size_t inputSize = 32;             // PC -> PLC: 16, 32, 64, 128 or 256 bytes
    size_t outputSize = 32;            // PLC -> PC: 16, 32, 64, 128 or 256 bytes
    uint32_t minUpdateTimeMs = 8;      // Connect requests with a faster update time are refused
    bool sendVlanTag = true;           // RT and alarm frames with 802.1Q priority 6
    bool promiscuous = false;          // see L2Options::promiscuous
    bool raiseThreadPriority = true;   // RX and cyclic threads at TIME_CRITICAL
    uint16_t rpcPort = 34964;          // tests only: another port when a controller runs on this PC
    std::shared_ptr<L2Port> port;      // tests only: use this port instead of opening `adapter`

    // Callbacks run on the stack's threads: keep them short and never call stop() from one.
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>& outputs, bool run)> onOutputs; // PLC wrote new data
    std::function<void(DeviceState)> onStateChanged;
    std::function<void()> onSignal; // DCP "flash LED" request (identify the device on site)
};

struct DeviceStats {
    uint64_t txFrames = 0;
    uint64_t rxFrames = 0;
    uint64_t lateCycles = 0;
    uint32_t maxTxPeriodUs = 0;
    uint64_t arConnects = 0;
    uint64_t arAborts = 0;
    std::string lastAbort;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // pimpl unique_ptr member of an exported class
#endif

class SOFTPN_API Device {
public:
    explicit Device(DeviceConfig config);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool start(std::string* error = nullptr);
    void stop();

    bool setInputData(const void* data, size_t len); // PC -> PLC, thread safe
    std::vector<uint8_t> outputData() const;         // PLC -> PC, thread safe (last valid values)
    bool controllerRun() const;
    DeviceState state() const;
    std::string stationName() const;
    DeviceStats stats() const;
    const DeviceConfig& config() const;

    static const char* stateName(DeviceState s);
    // "name | friendly name | MAC | IPv4" per adapter (empty + error text if Npcap is missing).
    static std::vector<std::string> listAdapters(std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace softpn
```

### Key Concepts
1. **States**: Stopped, WaitingForController, Connecting, DataRun, DataStop.
2. **`port` / `rpcPort`**: test hooks (memory cable, a controller on the same PC).
3. **Callbacks** run on stack threads: keep them short, never call `stop()` from one.

---

## Step 9.13.2: `pn_device.cpp`

**File:** `src/pn/pn_device.cpp`

### Purpose
Wiring and threads: RX (dispatch by FrameID), cyclic (DeadlineTimer: input frames, watchdog, DCP delays, LLDP), RPC (requests, ApplicationReady, timeouts).

### Implementation

```cpp
// softpn::Device: wires L2 port, DCP, LLDP, RPC, AR, cyclic and RTA together and owns the threads.
//
//   RX thread      L2Port::receive -> DCP / cyclic consumer / RTA
//   cyclic thread  1 ms tick: input-CR frames, data-hold watchdog, delayed DCP answers, LLDP every 5 s
//   RPC thread     UDP 34964: Connect / Write / Read / Control / Release, ApplicationReady, AR timeouts
#include "softeip/socket_compat.hpp" // first: winsock2.h before windows.h

#include "softpn/pn_device.hpp"

#include "softeip/periodic_timer.hpp"

#include "pn_ar.hpp"
#include "pn_cyclic.hpp"
#include "pn_dcp.hpp"
#include "pn_defs.hpp"
#include "pn_lldp.hpp"
#include "pn_model.hpp"
#include "pn_rpc.hpp"
#include "pn_rta.hpp"
#include "pn_station_store.hpp"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <thread>

namespace softpn {

namespace {

void raisePriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
}

} // namespace

struct Device::Impl {
    explicit Impl(DeviceConfig c) : cfg(std::move(c)), store(cfg.persistPath)
    {
        // The process image lives as long as the Device, so setInputData() works before start().
        CyclicEngine::Hooks ch;
        ch.send = [this](const std::vector<uint8_t>& f) { return send(f); };
        ch.onOutputs = [this](const std::vector<uint8_t>& out, bool run) {
            if (cfg.onOutputs)
                cfg.onOutputs(out, run);
            refreshState();
        };
        ch.onDataHoldExpired = [this] {
            if (ar)
                ar->abort(err::kRtaAbortConsumerDht, true,
                          "no cyclic frames from the controller (data-hold timer expired)");
        };
        cyclic = std::make_unique<CyclicEngine>(cfg.inputSize, cfg.outputSize, ch);
    }

    DeviceConfig cfg;
    DeviceModel model;
    StationStore store;
    std::shared_ptr<L2Port> port;
    std::unique_ptr<softeip::SocketLibrary> sockets;
    std::unique_ptr<DcpResponder> dcp;
    std::unique_ptr<CyclicEngine> cyclic;
    std::unique_ptr<RtaEngine> rta;
    std::unique_ptr<ArManager> ar;
    RpcEndpoint rpc;
    std::thread rxThread, txThread, rpcThread;
    std::atomic<bool> running{false};
    std::atomic<bool> lldpDirty{true};
    std::atomic<DeviceState> lastState{DeviceState::Stopped};

    void log(const std::string& m) const
    {
        if (cfg.onLog)
            cfg.onLog(m);
    }

    bool send(const std::vector<uint8_t>& frame) { return port && port->send(frame); }

    DeviceState computeState() const
    {
        if (!running || !ar)
            return DeviceState::Stopped;
        switch (ar->state()) {
        case ArState::Idle: return DeviceState::WaitingForController;
        case ArState::WaitPrmEnd:
        case ArState::WaitAppReadyCnf: return DeviceState::Connecting;
        case ArState::Data: return cyclic->controllerRun() ? DeviceState::DataRun : DeviceState::DataStop;
        }
        return DeviceState::Stopped;
    }

    void refreshState()
    {
        const DeviceState s = computeState();
        if (lastState.exchange(s) != s && cfg.onStateChanged)
            cfg.onStateChanged(s);
    }

    void rxLoop()
    {
        if (cfg.raiseThreadPriority)
            raisePriority();
        std::vector<uint8_t> frame;
        frame.reserve(kMaxFrameSize);
        while (running) {
            if (!port->receive(frame, 100))
                continue;
            EthHeader eth;
            if (!parseEthHeader(frame.data(), frame.size(), eth) || eth.etherType != kEtherTypeProfinet)
                continue; // LLDP from the neighbour is not evaluated
            const uint16_t id = frameIdOf(frame.data(), frame.size(), eth);
            if (id >= kFrameIdDcpHello && id <= kFrameIdDcpIdentifyRes)
                dcp->onFrame(eth, frame.data(), frame.size());
            else if (id >= kFrameIdRtClass1First && id <= kFrameIdRtClass1Last)
                cyclic->onFrame(eth, frame.data(), frame.size());
            else if (id == kFrameIdAlarmHigh || id == kFrameIdAlarmLow)
                rta->onFrame(eth, frame.data(), frame.size());
        }
    }

    void txLoop()
    {
        if (cfg.raiseThreadPriority)
            raisePriority();
        // Wake up exactly when the next input frame is due (one-shot high-resolution timer), or
        // every 2 ms for housekeeping: watchdog, delayed DCP answers, LLDP.
        softeip::DeadlineTimer timer;
        auto nextLldp = std::chrono::steady_clock::now();
        auto nextHousekeeping = nextLldp;
        while (running) {
            timer.waitUntil(std::min(cyclic->nextDue(), nextHousekeeping));
            const auto now = std::chrono::steady_clock::now();
            cyclic->tick(now);
            if (now < nextHousekeeping)
                continue;
            nextHousekeeping = now + std::chrono::milliseconds(2);
            dcp->poll(now);
            if (now >= nextLldp || lldpDirty.exchange(false)) {
                LldpInfo li;
                li.mac = port->info().mac;
                li.stationName = dcp->station().name;
                send(buildLldpFrame(li));
                nextLldp = now + std::chrono::milliseconds(lldp::kTxIntervalMs);
            }
        }
    }

    void rpcLoop()
    {
        while (running) {
            rpc.poll(10);
            ar->poll(std::chrono::steady_clock::now());
        }
    }
};

Device::Device(DeviceConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Device::~Device() { stop(); }

bool Device::start(std::string* error)
{
    Impl& d = *impl_;
    if (d.running)
        return true;
    const DeviceConfig& c = d.cfg;
    auto fail = [&](const std::string& msg) {
        if (error)
            *error = msg;
        d.log(msg);
        return false;
    };

    std::string err;
    if (!d.model.build(c.inputSize, c.outputSize, &err))
        return fail(err);
    std::string why;
    if (!isValidStationName(c.stationName, &why))
        return fail("invalid stationName '" + c.stationName + "': " + why);

    d.sockets = std::make_unique<softeip::SocketLibrary>();
    if (c.port) {
        d.port = c.port;
    } else {
        L2Options opt;
        opt.promiscuous = c.promiscuous;
        std::unique_ptr<L2Port> p = openL2Port(c.adapter, opt, &err);
        if (!p)
            return fail(err);
        d.port = std::move(p);
    }
    const AdapterInfo& nic = d.port->info();
    if (nic.ipv4 == 0)
        return fail("adapter " + nic.friendlyName + " has no IPv4 address: give the NIC a static IP");

    StationData station;
    if (!d.store.load(station) || station.name.empty())
        station.name = c.stationName;

    DcpResponder::Config dc;
    dc.mac = nic.mac;
    dc.vendorId = c.vendorId;
    dc.deviceId = c.deviceId;
    dc.vendorValue = c.deviceVendorValue;
    DcpResponder::Hooks dh;
    dh.send = [&d](const std::vector<uint8_t>& f) { return d.send(f); };
    dh.log = [&d](const std::string& m) { d.log(m); };
    dh.onSignal = [&d] {
        if (d.cfg.onSignal)
            d.cfg.onSignal();
    };
    dh.inOperation = [&d] { return d.ar && d.ar->state() == ArState::Data; };
    dh.persist = [&d](const StationData& s) {
        std::string e;
        if (!d.store.save(s, &e))
            d.log("DCP: " + e);
    };
    dh.onIdentityChanged = [&d] { d.lldpDirty = true; };
    d.dcp = std::make_unique<DcpResponder>(dc, dh);
    d.dcp->setStation(station);
    d.dcp->setIpSuite(nic.ipv4, nic.netmask, nic.gateway);

    RtaEngine::Hooks rh;
    rh.send = dh.send;
    rh.log = dh.log;
    rh.onErrorReceived = [&d](const PnioStatus& st) {
        d.ar->abort(err::kRtaAbortRequested, false, "aborted by the controller (RTA ERR " + st.str() + ")");
    };
    d.rta = std::make_unique<RtaEngine>(rh);

    ArConfig ac;
    ac.mac = nic.mac;
    ac.vendorId = c.vendorId;
    ac.deviceId = c.deviceId;
    ac.minUpdateTimeMs = c.minUpdateTimeMs;
    ac.vlan = c.sendVlanTag;
    ac.rpcPort = c.rpcPort;
    ac.im0.orderId = c.orderId;
    ac.im0.serialNumber = c.serialNumber;
    ac.im0.hardwareRevision = c.hardwareRevision;
    for (int i = 0; i < 3; ++i)
        ac.im0.swRevision[i] = c.softwareRevision[i];
    ArManager::Hooks ah;
    ah.log = dh.log;
    ah.stationName = [&d] { return d.dcp->station().name; };
    ah.startCyclic = [&d](const CyclicSetup& s) { d.cyclic->start(s); };
    ah.stopCyclic = [&d] { d.cyclic->stop(); };
    ah.setApplicationReady = [&d](bool r) { d.cyclic->setApplicationReady(r); };
    ah.armWatchdog = [&d] { d.cyclic->armWatchdog(); };
    ah.startRta = [&d](const RtaSetup& s) { d.rta->start(s); };
    ah.stopRta = [&d] { d.rta->stop(); };
    ah.sendRtaError = [&d](const PnioStatus& st) { d.rta->sendError(st); };
    ah.sendRpcRequest = [&d](const UdpAddress& to, const Uuid& obj, const Uuid& ifc, uint16_t op, const Uuid& act,
                             uint32_t seq, const std::vector<uint8_t>& body) {
        return d.rpc.sendRequest(to, obj, ifc, op, act, seq, body);
    };
    ah.stateChanged = [&d](ArState) { d.refreshState(); };
    d.ar = std::make_unique<ArManager>(ac, d.model, ah);

    d.rpc.setServerBoot(uint32_t(std::time(nullptr)));
    d.rpc.setRequestHandler([&d](const RpcRequest& req, std::vector<uint8_t>& res) { return d.ar->handleRequest(req, res); });
    d.rpc.setResponseHandler([&d](const RpcHeader& h, const std::vector<uint8_t>& body, const UdpAddress& from) {
        d.ar->handleResponse(h, body, from);
    });
    if (!d.rpc.open(c.rpcPort, &err)) {
        d.port.reset();
        return fail(err);
    }

    d.running = true;
    d.rxThread = std::thread([&d] { d.rxLoop(); });
    d.txThread = std::thread([&d] { d.txLoop(); });
    d.rpcThread = std::thread([&d] { d.rpcLoop(); });
    d.log("PROFINET device on " + nic.friendlyName + " (" + nic.mac.str() + ", " + ipToString(nic.ipv4) +
          "), NameOfStation '" + station.name + "', input " + std::to_string(c.inputSize) + " B / output " +
          std::to_string(c.outputSize) + " B");
    d.refreshState();
    return true;
}

void Device::stop()
{
    Impl& d = *impl_;
    if (!d.running)
        return;
    d.ar->abort(err::kRtaAbortDeactivated, true, "device stopped"); // RTA ERR while the port is still open
    d.running = false;
    d.port->interrupt();
    if (d.rxThread.joinable())
        d.rxThread.join();
    if (d.txThread.joinable())
        d.txThread.join();
    if (d.rpcThread.joinable())
        d.rpcThread.join();
    d.rpc.close();
    d.refreshState();
    d.port.reset();
    d.sockets.reset();
}

bool Device::setInputData(const void* data, size_t len)
{
    return impl_->cyclic->setInputData(data, len);
}

std::vector<uint8_t> Device::outputData() const
{
    return impl_->cyclic->outputData();
}

bool Device::controllerRun() const { return impl_->running && impl_->cyclic->controllerRun(); }
DeviceState Device::state() const { return impl_->computeState(); }
std::string Device::stationName() const { return impl_->dcp ? impl_->dcp->station().name : impl_->cfg.stationName; }
const DeviceConfig& Device::config() const { return impl_->cfg; }

DeviceStats Device::stats() const
{
    const Impl& d = *impl_;
    DeviceStats s;
    const CyclicStats c = d.cyclic->stats();
    s.txFrames = c.txFrames;
    s.rxFrames = c.rxFrames;
    s.lateCycles = c.lateCycles;
    s.maxTxPeriodUs = c.maxTxPeriodUs;
    if (d.ar) {
        s.arConnects = d.ar->connects();
        s.arAborts = d.ar->aborts();
        s.lastAbort = d.ar->lastAbort();
    }
    return s;
}

const char* Device::stateName(DeviceState s)
{
    switch (s) {
    case DeviceState::Stopped: return "Stopped";
    case DeviceState::WaitingForController: return "WaitingForController";
    case DeviceState::Connecting: return "Connecting";
    case DeviceState::DataRun: return "DataRun";
    case DeviceState::DataStop: return "DataStop";
    }
    return "?";
}

std::vector<std::string> Device::listAdapters(std::string* error)
{
    std::vector<std::string> out;
    for (const AdapterInfo& a : softpn::listAdapters(error))
        out.push_back(a.name + " | " + a.friendlyName + " | " + a.mac.str() + " | " +
                      (a.ipv4 ? ipToString(a.ipv4) : std::string("no IPv4")));
    return out;
}

} // namespace softpn
```

### Key Concepts
1. **Process image lives as long as the Device**: `setInputData()` before `start()` is kept.
2. **`stop()`** sends an RTA ERR first (the controller sees a clean abort), then joins the threads.
3. **TIME_CRITICAL** priority for RX and cyclic threads (option `raiseThreadPriority`).

---

## Step 9.13.3: `pn_device_demo.cpp`

**File:** `examples/pn_device_demo.cpp`

### Purpose
Echo device like `eip_adapter_demo`: outputs are echoed back from byte 4, bytes 0-3 are a heartbeat. Prints statistics every 10 s.

### Implementation

```cpp
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
```

### Key Concepts
1. **`--promisc 1`** if CODESYS/Proneta cannot find the device on a given NIC.
2. **`--priority 0`** to compare jitter with normal thread priority.

---

## Step 9.13.4: Add the facade and the demo

**File:** `CMakeLists.txt`

### Purpose
Append to the lists and add the executable:

### Implementation

```cmake
    src/pn/pn_device.cpp            # SOFTPN_SOURCES
    include/softpn/pn_device.hpp    # SOFTPN_HEADERS

add_executable(pn_device_demo examples/pn_device_demo.cpp)
target_link_libraries(pn_device_demo PRIVATE softpn)
softeip_warnings(pn_device_demo)
set_target_properties(pn_device_demo PROPERTIES FOLDER "examples" VS_DEBUGGER_COMMAND_ARGUMENTS "--list")
```

### Key Concepts
1. **Linux**: build, then `sudo setcap cap_net_raw+ep build/linux/bin/pn_device_demo`.

---

## Step 9.13.5: Gate B: discovery

### Purpose
The device must be visible and nameable before connections make sense.

### Procedure
```bat
pn_device_demo --adapter "Ethernet 2" --name softpn-device
```
From the controller side (CODESYS "Scan for devices", Proneta, or `pn_probe --adapter X --identify`
on a second PC): the device appears with vendor 0xFFFF / device 0x0001 and "Univision SoftPN".
Set a new name (`pn_probe --set-name <MAC> line1-camera`), restart the demo: the name is kept
(`softpn.ini`). `pn_probe --blink <MAC>` prints "DCP signal" in the demo.

---

## Verification Checklist

- [ ] `pn_device_demo --list` shows the adapters
- [ ] Gate B: identify, set name (persisted), blink
- [ ] Wireshark: DCP Identify response and LLDP every 5 s without malformed markers

---

## Next Phase
Continue to [Phase9.14-ControllerSimulator.md](Phase9.14-ControllerSimulator.md): Controller simulator (pn_controller_sim).
