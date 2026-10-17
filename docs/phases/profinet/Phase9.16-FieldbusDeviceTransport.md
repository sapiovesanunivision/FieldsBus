# Phase 9.16: PROFINET in softfb::FieldbusDevice

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Make PROFINET a transport of the common Hilscher-style API, so applications switch fieldbus by configuration: `Transport::ProfinetRt` in `FieldbusDevice`, `fb_device_demo --transport profinet`, and PROFINET inside `SoftFieldbus.dll`.

## Documentation References
- [One API for every fieldbus](../../../README.md#one-api-for-every-fieldbus)

---

## Step 9.16.1: `fieldbus_device.hpp`

**File:** `include/softfb/fieldbus_device.hpp`

### Purpose
`Transport::ProfinetRt` and `DeviceConfig::profinet` (adapter, station name, persist path, vendor/device ID, minimum update time, VLAN, promiscuous).

### Implementation

```cpp
// Transport-independent fieldbus device, modelled on the Hilscher cifX channel API.
//
// The application only sees two fixed byte areas, named from the PC's point of view:
//   input  area  PLC -> PC   ioRead()   (cf. xChannelIORead)
//   output area  PC -> PLC   ioWrite()  (cf. xChannelIOWrite)
// PC and PLC cast these bytes to agreed data types. Switching fieldbus is a config change:
//
//   softfb::DeviceConfig cfg;
//   cfg.transport = softfb::Transport::ModbusTcp;   // or EtherNetIP, ModbusUdp, ModbusTcpUdp, ProfinetRt
//   cfg.inputSize = 64; cfg.outputSize = 64;
//   softfb::FieldbusDevice dev(cfg);
//   dev.start();
//   dev.ioRead(0, &fromPlc, sizeof fromPlc);
//   dev.ioWrite(0, &toPlc, sizeof toPlc);
//
// Where the areas live on each bus:
//   EtherNet/IP  input area  = O->T output assembly (default 150, 32-bit run/idle header)
//                output area = T->O input assembly  (default 100)
//   Modbus       input area  = holding registers / coils
//                output area = input registers / discrete inputs
//   PROFINET RT  input area  = output submodule, slot 2 (controller -> device)
//                output area = input submodule,  slot 1 (device -> controller)
#pragma once

#include "softeip/eip_adapter.hpp" // IdentityInfo (header has no socket includes)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace softfb {

enum class Transport {
    EtherNetIP,
    ModbusTcp,
    ModbusUdp,
    ModbusTcpUdp, // one slave answering on both TCP and UDP
    ProfinetRt,   // PROFINET RT IO-Device (softpn; on Windows the build needs the Npcap SDK)
};

enum class DeviceState {
    Stopped,
    WaitingForMaster, // started, no PLC talking to us yet (EtherNet/IP scanner / Modbus client); name kept for API compatibility
    ConnectedIdle,    // EtherNet/IP: connection open, PLC in program/idle mode
    ConnectedRun,     // data exchange active (Modbus has no idle state)
};

struct DeviceConfig {
    Transport transport = Transport::EtherNetIP;
    std::string bindAddress = "0.0.0.0";
    size_t inputSize = 32;  // bytes PLC -> PC
    size_t outputSize = 32; // bytes PC -> PLC
    bool raiseThreadPriority = true;

    struct EtherNetIpOptions {
        uint16_t plcToPcInstance = 150; // O->T assembly = input area
        uint16_t pcToPlcInstance = 100; // T->O assembly = output area
        uint16_t configInstance = 151;
        uint32_t minRpiUs = 2000;
        softeip::IdentityInfo identity;
    } eip;

    struct ModbusOptions {
        uint16_t port = 502;
        uint8_t unitId = 0;          // 0 = any
        int outputsInHoldingAt = -1; // >= 0: output area also readable via FC03 from this register
        std::string vendorName = "SoftFieldbus";
        std::string productCode = "SoftMB";
        std::string revision = "1.0";
    } modbus;

    struct ProfinetOptions {
        std::string adapter;                       // NIC: friendly name, MAC, IPv4 or Npcap name (bindAddress unused)
        std::string stationName = "softpn-device"; // default NameOfStation
        std::string persistPath = "softpn.ini";    // NameOfStation set by the controller over DCP
        uint16_t vendorId = 0xFFFF;                // PLACEHOLDER until a PI vendor ID exists; must match the GSDML
        uint16_t deviceId = 0x0001;
        uint32_t minUpdateTimeMs = 8;
        bool sendVlanTag = true;
        bool promiscuous = false; // only if CODESYS / Proneta cannot find the device (see softpn::L2Options)
    } profinet;

    // Callbacks run on the network thread; keep them short.
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>& inputs)> onInputsChanged; // the PLC wrote new data
    std::function<void(DeviceState)> onStateChanged;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // pimpl unique_ptr member of an exported class
#endif

class SOFTFB_API FieldbusDevice {
public:
    explicit FieldbusDevice(DeviceConfig config);
    ~FieldbusDevice();
    FieldbusDevice(const FieldbusDevice&) = delete;
    FieldbusDevice& operator=(const FieldbusDevice&) = delete;

    bool start(std::string* error = nullptr);
    void stop();

    // Thread safe. Return false when offset+len is outside the area.
    bool ioRead(size_t offset, void* data, size_t len) const;  // input area  (PLC -> PC)
    bool ioWrite(size_t offset, const void* data, size_t len); // output area (PC -> PLC)

    DeviceState state() const;
    Transport transport() const;
    size_t inputSize() const;
    size_t outputSize() const;

    static const char* transportName(Transport t);
    static const char* stateName(DeviceState s);
    // Accepts "eip", "ethernetip", "modbus-tcp", "modbus-udp", "modbus" (= TCP+UDP), "profinet" / "pn".
    static bool parseTransport(const std::string& text, Transport& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace softfb
```

### Key Concepts
1. **Area mapping** (PC view): output area = slot 1 (PC -> PLC), input area = slot 2 (PLC -> PC).
2. **`bindAddress` is not used** for PROFINET: the adapter selects the NIC.

---

## Step 9.16.2: `fieldbus_device.cpp`

**File:** `src/fieldbus_device.cpp`

### Purpose
Starts a `softpn::Device` for `ProfinetRt`, maps the states and the areas. Compiled with `SOFTFB_HAS_PROFINET`; without it, selecting PROFINET fails in `start()` with a clear message.

### Implementation

```cpp
// FieldbusDevice: maps the PC-view process image onto softeip / softmb / softpn.
#include "softfb/fieldbus_device.hpp"

#include "softmb/modbus_server.hpp"
#ifdef SOFTFB_HAS_PROFINET
#include "softpn/pn_device.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>

namespace softfb {

struct FieldbusDevice::Impl {
    explicit Impl(DeviceConfig c) : cfg(std::move(c)), outputShadow(cfg.outputSize, 0) {}

    bool isEip() const { return cfg.transport == Transport::EtherNetIP; }
    bool isProfinet() const { return cfg.transport == Transport::ProfinetRt; }

    // Recomputes the state and reports changes (called from the network thread).
    void refreshState()
    {
        DeviceState s = computeState();
        DeviceState old = lastState.exchange(s);
        if (s != old && cfg.onStateChanged)
            cfg.onStateChanged(s);
    }

    DeviceState computeState() const
    {
        if (!started)
            return DeviceState::Stopped;
        if (eip) {
            if (!eip->outputConnected())
                return DeviceState::WaitingForMaster;
            return eip->plcInRun() ? DeviceState::ConnectedRun : DeviceState::ConnectedIdle;
        }
        if (mb)
            return mb->clientConnected() ? DeviceState::ConnectedRun : DeviceState::WaitingForMaster;
#ifdef SOFTFB_HAS_PROFINET
        if (pn) {
            switch (pn->state()) {
            case softpn::DeviceState::DataRun: return DeviceState::ConnectedRun;
            case softpn::DeviceState::DataStop: return DeviceState::ConnectedIdle;
            case softpn::DeviceState::Stopped: return DeviceState::Stopped;
            default: return DeviceState::WaitingForMaster; // no AR yet, or still parameterising
            }
        }
#endif
        return DeviceState::Stopped;
    }

    DeviceConfig cfg;
    std::unique_ptr<softeip::Adapter> eip;
    std::unique_ptr<softmb::ModbusServer> mb;
#ifdef SOFTFB_HAS_PROFINET
    std::unique_ptr<softpn::Device> pn;
#endif
    std::atomic<bool> started{false};
    std::atomic<DeviceState> lastState{DeviceState::Stopped};

    // softeip::Adapter only accepts a whole input image, so partial ioWrite()
    // calls go through a shadow copy of the output area.
    std::mutex shadowMutex;
    std::vector<uint8_t> outputShadow;
};

FieldbusDevice::FieldbusDevice(DeviceConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

FieldbusDevice::~FieldbusDevice() { stop(); }

bool FieldbusDevice::start(std::string* error)
{
    Impl& d = *impl_;
    if (d.started)
        return true;
    const DeviceConfig& c = d.cfg;

    if (d.isEip()) {
        softeip::AdapterConfig e;
        e.identity = c.eip.identity;
        e.bindAddress = c.bindAddress;
        // CIP names assemblies from the device's view: "input" = to the PLC.
        e.inputInstance = c.eip.pcToPlcInstance;
        e.outputInstance = c.eip.plcToPcInstance;
        e.configInstance = c.eip.configInstance;
        e.inputSize = c.outputSize;
        e.outputSize = c.inputSize;
        e.minRpiUs = c.eip.minRpiUs;
        e.raiseThreadPriority = c.raiseThreadPriority;
        e.onLog = c.onLog;
        e.onOutputs = [&d](const std::vector<uint8_t>& data, bool) {
            if (d.cfg.onInputsChanged)
                d.cfg.onInputsChanged(data);
            d.refreshState();
        };
        e.onConnectionChanged = [&d](bool) { d.refreshState(); };
        d.eip = std::make_unique<softeip::Adapter>(e);
        {
            std::lock_guard<std::mutex> lock(d.shadowMutex);
            d.eip->setInputData(d.outputShadow.data(), d.outputShadow.size());
        }
        d.started = true; // before start(): callbacks may fire immediately
        if (!d.eip->start(error)) {
            d.started = false;
            d.eip.reset();
            return false;
        }
    } else if (d.isProfinet()) {
#ifdef SOFTFB_HAS_PROFINET
        softpn::DeviceConfig p;
        p.adapter = c.profinet.adapter;
        p.stationName = c.profinet.stationName;
        p.persistPath = c.profinet.persistPath;
        p.vendorId = c.profinet.vendorId;
        p.deviceId = c.profinet.deviceId;
        p.minUpdateTimeMs = c.profinet.minUpdateTimeMs;
        p.sendVlanTag = c.profinet.sendVlanTag;
        p.promiscuous = c.profinet.promiscuous;
        // PROFINET names data from the device's view, like CIP: "input" = to the PLC.
        p.inputSize = c.outputSize;
        p.outputSize = c.inputSize;
        p.raiseThreadPriority = c.raiseThreadPriority;
        p.onLog = c.onLog;
        p.onOutputs = [&d](const std::vector<uint8_t>& data, bool) {
            if (d.cfg.onInputsChanged)
                d.cfg.onInputsChanged(data);
            d.refreshState();
        };
        p.onStateChanged = [&d](softpn::DeviceState) { d.refreshState(); };
        d.pn = std::make_unique<softpn::Device>(p);
        {
            std::lock_guard<std::mutex> lock(d.shadowMutex);
            d.pn->setInputData(d.outputShadow.data(), d.outputShadow.size());
        }
        d.started = true;
        if (!d.pn->start(error)) {
            d.started = false;
            d.pn.reset();
            return false;
        }
#else
        if (error)
            *error = "this build has no PROFINET support (configure CMake with SOFTPN_NPCAP_SDK)";
        return false;
#endif
    } else {
        softmb::ModbusServerConfig m;
        m.bindAddress = c.bindAddress;
        m.port = c.modbus.port;
        m.enableTcp = c.transport != Transport::ModbusUdp;
        m.enableUdp = c.transport != Transport::ModbusTcp;
        m.unitId = c.modbus.unitId;
        m.inputSize = c.inputSize;
        m.outputSize = c.outputSize;
        m.outputsInHoldingAt = c.modbus.outputsInHoldingAt;
        m.vendorName = c.modbus.vendorName;
        m.productCode = c.modbus.productCode;
        m.revision = c.modbus.revision;
        m.raiseThreadPriority = c.raiseThreadPriority;
        m.onLog = c.onLog;
        m.onInputsChanged = c.onInputsChanged;
        m.onConnectionChanged = [&d](bool) { d.refreshState(); };
        d.mb = std::make_unique<softmb::ModbusServer>(m);
        {
            std::lock_guard<std::mutex> lock(d.shadowMutex);
            d.mb->ioWrite(0, d.outputShadow.data(), d.outputShadow.size());
        }
        d.started = true;
        if (!d.mb->start(error)) {
            d.started = false;
            d.mb.reset();
            return false;
        }
    }
    d.refreshState();
    return true;
}

void FieldbusDevice::stop()
{
    Impl& d = *impl_;
    if (!d.started)
        return;
    if (d.eip)
        d.eip->stop(); // joins the network thread: no callbacks after this
    if (d.mb)
        d.mb->stop();
#ifdef SOFTFB_HAS_PROFINET
    if (d.pn)
        d.pn->stop();
#endif
    d.started = false;
    d.refreshState();
    d.eip.reset();
    d.mb.reset();
#ifdef SOFTFB_HAS_PROFINET
    d.pn.reset();
#endif
}

bool FieldbusDevice::ioRead(size_t offset, void* data, size_t len) const
{
    const Impl& d = *impl_;
    if (offset > d.cfg.inputSize || len > d.cfg.inputSize - offset)
        return false;
    if (d.eip) {
        std::vector<uint8_t> in = d.eip->outputData();
        std::memcpy(data, in.data() + offset, len);
        return true;
    }
    if (d.mb)
        return d.mb->ioRead(offset, data, len);
#ifdef SOFTFB_HAS_PROFINET
    if (d.pn) {
        std::vector<uint8_t> in = d.pn->outputData();
        std::memcpy(data, in.data() + offset, len);
        return true;
    }
#endif
    std::memset(data, 0, len); // not started: inputs read as zero
    return true;
}

bool FieldbusDevice::ioWrite(size_t offset, const void* data, size_t len)
{
    Impl& d = *impl_;
    if (offset > d.cfg.outputSize || len > d.cfg.outputSize - offset)
        return false;
    std::lock_guard<std::mutex> lock(d.shadowMutex);
    std::memcpy(d.outputShadow.data() + offset, data, len);
    if (d.eip)
        d.eip->setInputData(d.outputShadow.data(), d.outputShadow.size());
    else if (d.mb)
        d.mb->ioWrite(offset, data, len);
#ifdef SOFTFB_HAS_PROFINET
    else if (d.pn)
        d.pn->setInputData(d.outputShadow.data(), d.outputShadow.size());
#endif
    return true;
}

DeviceState FieldbusDevice::state() const { return impl_->computeState(); }
Transport FieldbusDevice::transport() const { return impl_->cfg.transport; }
size_t FieldbusDevice::inputSize() const { return impl_->cfg.inputSize; }
size_t FieldbusDevice::outputSize() const { return impl_->cfg.outputSize; }

const char* FieldbusDevice::transportName(Transport t)
{
    switch (t) {
    case Transport::EtherNetIP: return "EtherNet/IP";
    case Transport::ModbusTcp: return "Modbus TCP";
    case Transport::ModbusUdp: return "Modbus UDP";
    case Transport::ModbusTcpUdp: return "Modbus TCP+UDP";
    case Transport::ProfinetRt: return "PROFINET RT";
    }
    return "?";
}

const char* FieldbusDevice::stateName(DeviceState s)
{
    switch (s) {
    case DeviceState::Stopped: return "Stopped";
    case DeviceState::WaitingForMaster: return "WaitingForMaster";
    case DeviceState::ConnectedIdle: return "ConnectedIdle";
    case DeviceState::ConnectedRun: return "ConnectedRun";
    }
    return "?";
}

bool FieldbusDevice::parseTransport(const std::string& text, Transport& out)
{
    std::string t = text;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (t == "eip" || t == "ethernetip" || t == "ethernet/ip")
        out = Transport::EtherNetIP;
    else if (t == "modbus-tcp" || t == "mbtcp")
        out = Transport::ModbusTcp;
    else if (t == "modbus-udp" || t == "mbudp")
        out = Transport::ModbusUdp;
    else if (t == "modbus")
        out = Transport::ModbusTcpUdp;
    else if (t == "profinet" || t == "pn" || t == "profinet-rt")
        out = Transport::ProfinetRt;
    else
        return false;
    return true;
}

} // namespace softfb
```

### Key Concepts
1. **State mapping**: DataRun -> ConnectedRun, DataStop -> ConnectedIdle, no AR / parameterising -> WaitingForMaster.
2. **Shadow copy**: partial `ioWrite()` calls are merged before `setInputData()`, as for EtherNet/IP.

---

## Step 9.16.3: `fb_device_demo.cpp`

**File:** `examples/fb_device_demo.cpp`

### Purpose
`--transport profinet --adapter NIC --station NAME`.

### Implementation

```cpp
// Same application on any fieldbus: the transport is just a command-line option.
//   input area  (PLC -> PC)  printed whenever the PLC changes it
//   output area (PC -> PLC)  = echo of the input area, bytes 0..3 replaced by a heartbeat counter
//
// Usage: fb_device_demo --transport eip|modbus-tcp|modbus-udp|modbus|profinet
//                       [--bind IP] [--in-size N] [--out-size N] [--port N] [--unit N]
//                       [--adapter NIC] [--station NAME]   (PROFINET; sizes 16/32/64/128/256)
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
        else if (k == "--adapter")
            cfg.profinet.adapter = v;
        else if (k == "--station")
            cfg.profinet.stationName = v;
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
```

### Key Concepts
1. **Sizes**: `--in-size` / `--out-size` must be a GSDML variant (default 64 is one).

---

## Step 9.16.4: Link softpn into the common API and SoftFieldbus.dll

**File:** `CMakeLists.txt`

### Purpose
At the end of `if(SOFTPN_ENABLED)`, and inside `if(SOFTFIELDBUS_SHARED)` before `softeip_warnings(softfieldbus_shared)`:

### Implementation

```cmake
    # PROFINET as a transport of the common API (softfb::FieldbusDevice, SoftFieldbus.Net).
    target_link_libraries(softfieldbus PUBLIC softpn)
    target_compile_definitions(softfieldbus PUBLIC SOFTFB_HAS_PROFINET=1)
endif()

# ... inside if(SOFTFIELDBUS_SHARED):
    if(SOFTPN_ENABLED)
        target_sources(softfieldbus_shared PRIVATE ${SOFTPN_SOURCES})
        target_include_directories(softfieldbus_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/pn)
        target_compile_definitions(softfieldbus_shared PUBLIC SOFTFB_HAS_PROFINET=1)
        if(WIN32)
            target_include_directories(softfieldbus_shared PRIVATE "${SOFTPN_NPCAP_SDK}/Include")
            target_link_libraries(softfieldbus_shared PRIVATE "${SOFTPN_NPCAP_SDK}/Lib/x64/wpcap.lib" iphlpapi delayimp)
            target_link_options(softfieldbus_shared PRIVATE "/DELAYLOAD:wpcap.dll")
        endif()
    endif()
```

### Key Concepts
1. **`SOFTFB_HAS_PROFINET` is PUBLIC**: consumers of the DLL see the same `#ifdef`s.

---

## Step 9.16.5: Reference: the complete CMakeLists.txt

**File:** `CMakeLists.txt`

### Purpose
For reference: the complete file after all phases, to compare with yours.

### Implementation

```cmake
cmake_minimum_required(VERSION 3.16)
project(SoftFieldbus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SOFTEIP_SHARED "Build softeip as a native C++ DLL / shared library instead of a static lib" OFF)
option(SOFTFIELDBUS_SHARED "Also build SoftFieldbus.dll (softeip + softmb + softfb in one DLL, as in Dev.3); mb_client_test links it" OFF)
option(SOFTFIELDBUS_ANALYZE "MSVC: compile our native targets with /analyze /WX (catches what Dev.3's code analysis flags)" OFF)
if(MSVC)
    set(_dotnet_default ON)
else()
    set(_dotnet_default OFF)
endif()
option(SOFTEIP_BUILD_DOTNET "Build the C++/CLI .NET wrapper SoftEip.Net.dll (MSVC + Visual Studio generator)" ${_dotnet_default})
set(SOFTEIP_DOTNET_FRAMEWORK "net8.0" CACHE STRING
    "Wrapper target: net8.0 / net10.0 (.NET for WPF, WinUI, MAUI-Windows) or v4.8 (.NET Framework)")

# All executables and DLLs side by side: build/<preset>/bin/<Config>/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Threads REQUIRED)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Warning flags for our own native targets (kept PRIVATE so they don't leak into the /clr wrapper).
function(softeip_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /MP)
        if(SOFTFIELDBUS_ANALYZE)
            target_compile_options(${target} PRIVATE /analyze /analyze:external- /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

set(SOFTEIP_HEADERS
    include/softeip/eip_adapter.hpp
    include/softeip/eip_scanner.hpp
    include/softeip/export.hpp
    include/softeip/bytes.hpp
    include/softeip/cip_defs.hpp
    include/softeip/socket_compat.hpp
)
if(SOFTEIP_SHARED)
    add_library(softeip SHARED src/eip_adapter.cpp src/eip_scanner.cpp ${SOFTEIP_HEADERS})
    target_compile_definitions(softeip PUBLIC SOFTEIP_SHARED PRIVATE SOFTEIP_BUILDING_DLL)
else()
    add_library(softeip STATIC src/eip_adapter.cpp src/eip_scanner.cpp ${SOFTEIP_HEADERS})
endif()
source_group("Header Files" FILES ${SOFTEIP_HEADERS})
target_include_directories(softeip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(softeip PUBLIC Threads::Threads)
softeip_warnings(softeip)

if(WIN32)
    target_link_libraries(softeip PUBLIC ws2_32 winmm)
    target_compile_definitions(softeip PUBLIC _WIN32_WINNT=0x0601)
endif()

add_executable(eip_adapter_demo examples/eip_adapter_demo.cpp)
target_link_libraries(eip_adapter_demo PRIVATE softeip)
softeip_warnings(eip_adapter_demo)

add_executable(eip_scanner_sim tools/eip_scanner_sim.cpp)
target_link_libraries(eip_scanner_sim PRIVATE softeip)
softeip_warnings(eip_scanner_sim)

# ---- Modbus (softmb): SERVER (PC holds the registers) + CLIENT (PC polls a PLC) ----
add_library(softmb STATIC
    src/modbus_server.cpp src/modbus_client.cpp src/modbus_client_poller.cpp
    include/softmb/modbus_server.hpp include/softmb/modbus_slave.hpp include/softmb/modbus_defs.hpp
    include/softmb/modbus_client.hpp include/softmb/modbus_client_poller.hpp)
target_include_directories(softmb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(softmb PUBLIC Threads::Threads)
softeip_warnings(softmb)
if(WIN32)
    target_link_libraries(softmb PUBLIC ws2_32 winmm)
    target_compile_definitions(softmb PUBLIC _WIN32_WINNT=0x0601)
endif()

add_executable(mb_server_demo examples/mb_server_demo.cpp)
target_link_libraries(mb_server_demo PRIVATE softmb)
softeip_warnings(mb_server_demo)

add_executable(mb_client_test tools/mb_client_test.cpp)
softeip_warnings(mb_client_test)

add_executable(mb_client tools/mb_client.cpp)
target_link_libraries(mb_client PRIVATE softmb)
softeip_warnings(mb_client)

# ---- Transport-independent device (Hilscher-style process image) ------------
add_library(softfieldbus STATIC src/fieldbus_device.cpp include/softfb/fieldbus_device.hpp)
target_link_libraries(softfieldbus PUBLIC softeip softmb)
softeip_warnings(softfieldbus)

add_executable(fb_device_demo examples/fb_device_demo.cpp)
target_link_libraries(fb_device_demo PRIVATE softfieldbus)
softeip_warnings(fb_device_demo)

# ---- PROFINET RT device (softpn): raw Ethernet via the softpn_ndis driver or Npcap (Windows),
#      AF_PACKET (Linux) ----
set(SOFTPN_NPCAP_SDK "$ENV{NPCAP_SDK}" CACHE PATH "Npcap SDK folder (Include/, Lib/x64/); empty = only the softpn_ndis driver backend on Windows")
set(SOFTPN_ENABLED OFF)
set(SOFTPN_HAS_NPCAP OFF)
if(WIN32)
    set(SOFTPN_ENABLED ON)
    if(SOFTPN_NPCAP_SDK AND EXISTS "${SOFTPN_NPCAP_SDK}/Include/pcap.h")
        set(SOFTPN_HAS_NPCAP ON)
    else()
        message(STATUS "softpn: no Npcap SDK (SOFTPN_NPCAP_SDK), only the softpn_ndis driver backend")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SOFTPN_ENABLED ON)
endif()

# Windows raw-Ethernet backends for a target that compiles the softpn sources.
function(softpn_windows_l2 target scope)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/drivers/softpn_ndis/inc)
    target_link_libraries(${target} ${scope} iphlpapi)
    if(SOFTPN_HAS_NPCAP)
        target_compile_definitions(${target} PRIVATE SOFTPN_HAS_NPCAP=1)
        target_include_directories(${target} PRIVATE "${SOFTPN_NPCAP_SDK}/Include")
        target_link_libraries(${target} ${scope} "${SOFTPN_NPCAP_SDK}/Lib/x64/wpcap.lib" delayimp)
        # wpcap.dll is loaded on first use from System32\Npcap: no Npcap = error message, not a crash.
        if(scope STREQUAL "PUBLIC")
            target_link_options(${target} INTERFACE "/DELAYLOAD:wpcap.dll")
        else()
            target_link_options(${target} PRIVATE "/DELAYLOAD:wpcap.dll")
        endif()
    endif()
endfunction()

if(SOFTPN_ENABLED)
    set(SOFTPN_SOURCES
        src/pn/pn_frame.cpp
        src/pn/l2_port_memory.cpp
        src/pn/pn_station_store.cpp
        src/pn/pn_dcp.cpp
        src/pn/pn_lldp.cpp
        src/pn/pn_rpc.cpp
        src/pn/pn_blocks.cpp
        src/pn/pn_model.cpp
        src/pn/pn_cyclic.cpp
        src/pn/pn_rta.cpp
        src/pn/pn_ar.cpp
        src/pn/pn_device.cpp
    )
    if(WIN32)
        list(APPEND SOFTPN_SOURCES src/pn/l2_port_windows.cpp src/pn/l2_port_ndis.cpp)
        if(SOFTPN_HAS_NPCAP)
            list(APPEND SOFTPN_SOURCES src/pn/l2_port_npcap.cpp)
        endif()
    else()
        list(APPEND SOFTPN_SOURCES src/pn/l2_port_linux.cpp)
    endif()
    set(SOFTPN_HEADERS
        include/softpn/l2_port.hpp
        src/pn/pn_defs.hpp
        src/pn/pn_frame.hpp
        src/pn/pn_station_store.hpp
        src/pn/pn_dcp.hpp
        src/pn/pn_lldp.hpp
        src/pn/pn_uuid.hpp
        src/pn/pn_rpc.hpp
        src/pn/pn_blocks.hpp
        src/pn/pn_model.hpp
        src/pn/pn_cyclic.hpp
        src/pn/pn_rta.hpp
        src/pn/pn_ar.hpp
        include/softpn/pn_device.hpp
    )
    if(WIN32)
        list(APPEND SOFTPN_HEADERS src/pn/l2_port_windows.hpp drivers/softpn_ndis/inc/softpn_ndis_ioctl.h)
    endif()
    add_library(softpn STATIC ${SOFTPN_SOURCES} ${SOFTPN_HEADERS})
    source_group("Header Files" FILES ${SOFTPN_HEADERS})
    target_include_directories(softpn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/pn)
    target_link_libraries(softpn PUBLIC softeip)
    softeip_warnings(softpn)
    if(WIN32)
        softpn_windows_l2(softpn PUBLIC)
    endif()
    set_target_properties(softpn PROPERTIES FOLDER "native")

    add_executable(pn_device_demo examples/pn_device_demo.cpp)
    target_link_libraries(pn_device_demo PRIVATE softpn)
    softeip_warnings(pn_device_demo)

    add_executable(pn_probe tools/pn_probe.cpp)
    target_include_directories(pn_probe PRIVATE src/pn)
    target_link_libraries(pn_probe PRIVATE softpn)
    softeip_warnings(pn_probe)

    # The controller simulator class is shared by the CLI tool and the offline self-test.
    add_library(pn_controller STATIC tools/pn_controller.cpp tools/pn_controller.hpp)
    target_include_directories(pn_controller PUBLIC src/pn tools)
    target_link_libraries(pn_controller PUBLIC softpn)
    softeip_warnings(pn_controller)

    add_executable(pn_controller_sim tools/pn_controller_sim.cpp)
    target_link_libraries(pn_controller_sim PRIVATE pn_controller)
    softeip_warnings(pn_controller_sim)

    add_executable(pn_selftest tools/pn_selftest.cpp)
    target_link_libraries(pn_selftest PRIVATE pn_controller)
    target_compile_definitions(pn_selftest PRIVATE
        SOFTPN_DEFAULT_GSDML="${CMAKE_CURRENT_SOURCE_DIR}/gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml")
    softeip_warnings(pn_selftest)
    enable_testing()
    add_test(NAME pn_selftest COMMAND pn_selftest)

    set_target_properties(pn_controller pn_probe pn_controller_sim pn_selftest PROPERTIES FOLDER "tools")
    set_target_properties(pn_device_demo PROPERTIES FOLDER "examples"
        VS_DEBUGGER_COMMAND_ARGUMENTS "--list")
    set_target_properties(pn_probe PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "--list")

    # PROFINET as a transport of the common API (softfb::FieldbusDevice, SoftFieldbus.Net).
    target_link_libraries(softfieldbus PUBLIC softpn)
    target_compile_definitions(softfieldbus PUBLIC SOFTFB_HAS_PROFINET=1)
endif()

# ---- Everything in one DLL: SoftFieldbus.dll (the Dev.3 SoftFieldbus*.dll is built from the same sources) ----
if(SOFTFIELDBUS_SHARED)
    add_library(softfieldbus_shared SHARED
        src/eip_adapter.cpp src/eip_scanner.cpp src/modbus_server.cpp src/modbus_client.cpp
        src/modbus_client_poller.cpp src/fieldbus_device.cpp)
    # Import library in its own folder: on Windows SoftFieldbus.lib and the static softfieldbus.lib are the same file name.
    set_target_properties(softfieldbus_shared PROPERTIES OUTPUT_NAME SoftFieldbus FOLDER "native"
                          ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/shared-lib
                          CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_include_directories(softfieldbus_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(softfieldbus_shared PUBLIC SOFTFIELDBUS_SHARED PRIVATE SOFTFIELDBUS_BUILDING_DLL)
    target_link_libraries(softfieldbus_shared PUBLIC Threads::Threads)
    if(WIN32)
        target_link_libraries(softfieldbus_shared PUBLIC ws2_32 winmm)
        target_compile_definitions(softfieldbus_shared PUBLIC _WIN32_WINNT=0x0601)
    endif()
    if(SOFTPN_ENABLED)
        target_sources(softfieldbus_shared PRIVATE ${SOFTPN_SOURCES})
        target_include_directories(softfieldbus_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/pn)
        target_compile_definitions(softfieldbus_shared PUBLIC SOFTFB_HAS_PROFINET=1)
        if(WIN32)
            softpn_windows_l2(softfieldbus_shared PRIVATE)
        endif()
    endif()
    softeip_warnings(softfieldbus_shared)
    target_link_libraries(mb_client_test PRIVATE softfieldbus_shared) # catches missing exports
else()
    target_link_libraries(mb_client_test PRIVATE softmb)
endif()

# Visual Studio solution niceties
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT eip_adapter_demo)
set_target_properties(softeip PROPERTIES FOLDER "native")
set_target_properties(eip_adapter_demo PROPERTIES FOLDER "examples")
set_target_properties(eip_scanner_sim PROPERTIES FOLDER "tools"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--target 127.0.0.1 --rpi-ms 10 --seconds 5 --local-port 2223")
set_target_properties(softmb softfieldbus PROPERTIES FOLDER "native")
set_target_properties(fb_device_demo PROPERTIES FOLDER "examples"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--transport modbus --unit 1")
set_target_properties(mb_server_demo PROPERTIES FOLDER "examples")
set_target_properties(mb_client PROPERTIES FOLDER "tools"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--host 127.0.0.1 read-holding 0 10")
set_target_properties(mb_client_test PROPERTIES FOLDER "tools"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--target 127.0.0.1 --transport tcp")

if(SOFTEIP_BUILD_DOTNET)
    if(NOT MSVC OR NOT CMAKE_GENERATOR MATCHES "Visual Studio")
        message(WARNING "SOFTEIP_BUILD_DOTNET needs MSVC and a Visual Studio generator - skipping .NET wrapper")
    else()
        add_subdirectory(dotnet)
    endif()
endif()
```

### Key Concepts
1. **Build variants checked**: static `/W4`, `SoftFieldbus.dll` + `/analyze /WX`, .NET wrapper, no SDK, Linux.

---

## Verification Checklist

- [ ] `fb_device_demo --transport profinet --adapter "Ethernet 2" --station softpn-device` reaches ConnectedRun against the simulator or CODESYS
- [ ] Build without `SOFTPN_NPCAP_SDK`: everything builds; `--transport profinet` prints "this build has no PROFINET support"
- [ ] `cmake --preset vs2026-analyze` with `-DSOFTPN_NPCAP_SDK=...` builds `SoftFieldbus.dll` with `/analyze /WX` clean
- [ ] EtherNet/IP and Modbus regression tests still pass

---

## Next Phase
Continue to [Phase9.17-DotNetWrapper.md](Phase9.17-DotNetWrapper.md): PROFINET in SoftFieldbus.Net (C#).
