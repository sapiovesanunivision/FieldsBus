# Phase 10a: L2 backend option in `softfb::DeviceConfig`

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repository: **FieldsBus**,
> branch `feature/profinet-poc`. **Status: implemented 2026-10-21** (uncommitted), code blocks = the files. Also
> fixed a C6001 false positive in `l2_port_ndis.cpp` that broke the `/analyze /WX` build (step 10a.6).

## Goal
Let users of the common API (`softfb::FieldbusDevice`, SoftFieldbus.Net, and later the Dev.3 service) choose the
raw-Ethernet backend: the softpn_ndis driver, Npcap, or automatic. Today only `softpn::DeviceConfig::l2Backend`
has this setting, so the service could only use the `SOFTPN_L2_BACKEND` environment variable. Also make the error
say what to install when no backend is present.

## Documentation References
- [Phase 9.20 - NDIS backend](../profinet/Phase9.20-NdisBackend.md) (`L2Backend`, `resolve()`)
- [Phase 9.16 - FieldbusDevice transport](../profinet/Phase9.16-FieldbusDeviceTransport.md)
- [Phase 9.17 - .NET wrapper](../profinet/Phase9.17-DotNetWrapper.md)

---

## Step 10a.1: `ProfinetOptions::l2Backend`

**File:** `include/softfb/fieldbus_device.hpp`

### Purpose
The service reads `Parameters\profinet\L2Backend` and needs a field to pass it on. A string keeps
`fieldbus_device.hpp` free of softpn headers. Consumers of the Dev.3 DLL include this header without
`SOFTFB_HAS_PROFINET`.

### Implementation

```cpp
// enum class Transport: replace the comment of ProfinetRt (the Npcap SDK is no longer required)
    ProfinetRt,   // PROFINET RT IO-Device (softpn; Windows: softpn_ndis driver or Npcap, see ProfinetOptions::l2Backend)

// struct ProfinetOptions: complete struct after the change
    struct ProfinetOptions {
        std::string adapter;                       // NIC: friendly name, MAC, IPv4 or Npcap name (bindAddress unused)
        std::string stationName = "softpn-device"; // default NameOfStation
        std::string persistPath = "softpn.ini";    // NameOfStation set by the controller over DCP
        uint16_t vendorId = 0xFFFF;                // PLACEHOLDER until a PI vendor ID exists; must match the GSDML
        uint16_t deviceId = 0x0001;
        uint32_t minUpdateTimeMs = 8;
        bool sendVlanTag = true;
        bool promiscuous = false; // only if CODESYS / Proneta cannot find the device (see softpn::L2Options)
        // Windows raw-Ethernet backend: "auto" (SOFTPN_L2_BACKEND if set, else the softpn_ndis driver if it is
        // running, else Npcap), "ndis" or "npcap". Linux ignores it.
        std::string l2Backend = "auto";
    } profinet;
```

### Key Concepts
1. **Default `auto`**: existing programs behave as before (`softpn::resolve()`).
2. **ABI**: the struct grows by one member. The Dev.3 DLL and the service are always built together, so this is safe.

---

## Step 10a.2: Pass it to softpn

**File:** `src/fieldbus_device.cpp`, `FieldbusDevice::start()`, PROFINET branch

### Purpose
Map the string onto `softpn::DeviceConfig::l2Backend`. An unknown value stops `start()` with a clear message, as an
unknown transport does.

### Implementation

```cpp
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
        if (!softpn::parseL2Backend(c.profinet.l2Backend, p.l2Backend)) {
            if (error)
                *error = "unknown PROFINET L2 backend '" + c.profinet.l2Backend + "' (auto, ndis, npcap)";
            return false;
        }
        // PROFINET names data from the device's view, like CIP: "input" = to the PLC.
        p.inputSize = c.outputSize;
        p.outputSize = c.inputSize;
        // ... unchanged from here (raiseThreadPriority, onLog, onOutputs, onStateChanged, start)
#else
        if (error)
            *error = "this build has no PROFINET support (softpn is built on Windows and Linux only)";
        return false;
#endif
```

### Key Concepts
1. **`parseL2Backend`** is declared in `include/softpn/l2_port.hpp`, which `pn_device.hpp` already includes.
2. **Return before anything is created**: `d.started` is still false, so `stop()` has nothing to undo.
3. **The `#else` text**: the old one pointed to `SOFTPN_NPCAP_SDK`, but since Phase 9.20 softpn is built on Windows
   without the SDK.

---

## Step 10a.3: Name both installs when `auto` finds neither

**File:** `src/pn/l2_port_windows.cpp`, `openL2Port()`

### Purpose
With `auto`, a PC that has neither the driver nor Npcap falls back to Npcap. The error then mentions only Npcap
("Npcap is not installed ..."). The service puts this text in the event log and in `errorDescription`, so it must
name both ways to fix it.

### Implementation

```cpp
std::unique_ptr<L2Port> openL2Port(const std::string& selector, const L2Options& options, std::string* error)
{
    const L2Backend backend = resolve(options.backend);
    std::vector<AdapterInfo> list = listAdapters(backend, error);
    if (list.empty()) {
        if (error && error->empty())
            *error = "no Ethernet adapter found";
        else if (error && options.backend == L2Backend::Auto && backend == L2Backend::Npcap)
            *error += " - or install and start the softpn_ndis driver (net start SoftPnNdis)";
        return nullptr;
    }
    // ... unchanged from here
```

### Key Concepts
1. **`ndis` / `npcap` chosen explicitly**: the message names only that backend. Its text already says what to do:
   "softpn_ndis driver not running (install it, then: net start SoftPnNdis)", or "Npcap is not installed (https://npcap.com) ...".
2. **Access**: the driver allows administrators and services only. The service (account `.\ProInspect` or
   LocalSystem) is fine. A console `-process` run with the `ndis` backend needs an elevated prompt.

---

## Step 10a.4: .NET wrapper

**Files:** `dotnet/SoftFieldbusNet.h`, `dotnet/SoftFieldbusNet.cpp`

### Purpose
Keep the C# API at the same level as the native one (Phase 9.17).

### Implementation

```cpp
// SoftFieldbusNet.h, FieldbusDeviceConfig, after ProfinetPromiscuous
    /// Windows raw-Ethernet backend: "auto" (driver if running, else Npcap), "ndis" or "npcap".
    property String^ ProfinetL2Backend;

// SoftFieldbusNet.cpp, toNative(), after n.profinet.promiscuous
    if (c->ProfinetL2Backend != nullptr)
        n.profinet.l2Backend = toStd(c->ProfinetL2Backend);

// SoftFieldbusNet.cpp, FieldbusDeviceConfig::FieldbusDeviceConfig(), after ProfinetPromiscuous
    ProfinetL2Backend = gcnew String(d.profinet.l2Backend.c_str());
```

### Key Concepts
1. **Defaults from the native struct** ("single source of truth"), as for the other properties.
2. **Validation stays native**: an unknown string fails in `Start()` with the message from step 10a.2.

---

## Step 10a.5: `fb_device_demo --backend`

**File:** `examples/fb_device_demo.cpp`

### Purpose
Lets you test the option from the command line without the service.

### Implementation

```cpp
// usage comment at the top
//                       [--adapter NIC] [--station NAME] [--backend auto|ndis|npcap]   (PROFINET; sizes 16/32/64/128/256)

// argument loop, after --station
        else if (k == "--backend")
            cfg.profinet.l2Backend = v;
```

### Key Concepts
1. **Same option name** as `pn_device_demo`, `pn_probe` and `pn_controller_sim`.

---

## Step 10a.6: `/analyze` false positive in the NDIS port destructor

**File:** `src/pn/l2_port_ndis.cpp`, `NdisPort::~NdisPort()`

### Purpose
`vs2026-analyze-debug` stopped with C6001 "Using uninitialized memory `slots_.ov.hEvent`" (line 138) since the
reopen fix of `c1324d9`. The events are created in the constructor (and `ov{}` zero-initializes them), so the
warning is false. But Dev.3 builds Debug with code analysis, so it would block 10e.

### Implementation

```cpp
    ~NdisPort() override
    {
        // copy the events first: /analyze (C6001) cannot follow them through drainReads()
        HANDLE events[kPendingReads] = {};
        for (size_t i = 0; i < kPendingReads; ++i)
            events[i] = slots_[i].ov.hEvent;
        drainReads();
        for (HANDLE e : events) {
            if (e)
                CloseHandle(e);
        }
        CloseHandle(h_);
        // ... unchanged
```

---

## Verification Checklist

Results 2026-10-21, dev PC (VS 2026, Npcap installed, no softpn_ndis driver):

- [x] `vs2026-release`: 0 warnings, including SoftFieldbus.Net and the C# samples
- [x] `vs2026-analyze-debug` (`/analyze /WX`, SoftFieldbus.dll): 0 warnings after step 10a.6
- [x] `pn_selftest`: 112/112 PASS
- [x] `--backend bogus` -> "start failed: unknown PROFINET L2 backend 'bogus' (auto, ndis, npcap)"
- [x] `--backend npcap` -> "PROFINET device on Ethernet (9c:2d:cd:4d:7d:be, 10.0.0.168) ...", state WaitingForMaster
- [x] `--backend ndis` -> "start failed: softpn_ndis driver not running (install it, then: net start SoftPnNdis)"
- [ ] `--backend auto` on a PC with neither: the message names Npcap and the driver (needs a PC without Npcap: test VM)
- [ ] Linux g++ build (no Windows code changed except `l2_port_*`; run with the next Linux pass)
- [ ] C# sample run with `ProfinetL2Backend` (built, not run)

---

## Next Phase
Continue to [Phase10b-ProInspectGsdml.md](Phase10b-ProInspectGsdml.md): GSDML for the ProInspect service.
