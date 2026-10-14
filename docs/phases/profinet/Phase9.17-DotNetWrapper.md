# Phase 9.17: PROFINET in SoftFieldbus.Net (C#)

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Expose the transport to WPF / WinUI / MAUI applications: `FieldbusTransport.ProfinetRt` and the `Profinet*` configuration properties, plus the C# sample.

## Documentation References
- [SoftFieldbus.Net usage](../../../README.md#using-it-from-c-wpf--winui--maui-windows)

---

## Step 9.17.1: `SoftFieldbusNet.h`

**File:** `dotnet/SoftFieldbusNet.h`

### Purpose
Enum value 4 and the configuration properties.

### Implementation

```cpp
// SoftFieldbus.Net - .NET wrapper (C++/CLI) around softfb::FieldbusDevice.
// One Hilscher-style API for every transport (EtherNet/IP, Modbus TCP/UDP, PROFINET RT).
//
// C# usage:
//   var cfg = new FieldbusDeviceConfig { Transport = FieldbusTransport.ModbusTcp, InputSize = 64, OutputSize = 64 };
//   using var dev = new FieldbusDevice(cfg);
//   dev.InputsChanged += (s, e) => ...;    // raised on the network thread!
//   dev.Start();
//   byte[] fromPlc = dev.IoRead();         // input area  (PLC -> PC)
//   dev.IoWrite(0, toPlc);                 // output area (PC -> PLC)
#pragma once

#include "softfb/fieldbus_device.hpp"

#include <string>
#include <vector>

namespace SoftFieldbus {

using namespace System;

// Values mirror softfb::Transport / softfb::DeviceState (checked in SoftFieldbusNet.cpp).
public enum class FieldbusTransport {
    EtherNetIP = 0,
    ModbusTcp = 1,
    ModbusUdp = 2,
    ModbusTcpUdp = 3,
    /// PROFINET RT IO-Device. Needs Npcap and a build configured with SOFTPN_NPCAP_SDK.
    ProfinetRt = 4,
};

public enum class FieldbusState {
    Stopped = 0,
    WaitingForMaster = 1,
    ConnectedIdle = 2,
    ConnectedRun = 3,
};

/// Configuration, read when Start() is called.
public ref class FieldbusDeviceConfig sealed {
public:
    FieldbusDeviceConfig();

    property FieldbusTransport Transport;
    /// IP of the NIC facing the PLC, or "0.0.0.0" for all NICs.
    property String^ BindAddress;
    /// Bytes PLC -> PC (input area, read with IoRead).
    property int InputSize;
    /// Bytes PC -> PLC (output area, written with IoWrite).
    property int OutputSize;
    property bool RaiseThreadPriority;

    // EtherNet/IP
    property int EipPlcToPcInstance;
    property int EipPcToPlcInstance;
    property int EipConfigInstance;
    property int EipMinRpiMicroseconds;
    property int VendorId;
    property int DeviceType;
    property int ProductCode;
    property int RevisionMajor;
    property int RevisionMinor;
    property UInt32 SerialNumber;
    property String^ ProductName;

    // Modbus
    property int ModbusPort;
    /// 0 = answer any unit id.
    property int ModbusUnitId;
    /// >= 0: output area also readable via FC03 from this holding register; -1 = off.
    property int ModbusOutputsInHoldingAt;
    property String^ ModbusVendorName;
    property String^ ModbusProductCode;
    property String^ ModbusRevision;

    // PROFINET RT (InputSize / OutputSize must be 16, 32, 64, 128 or 256)
    /// NIC facing the PLC: friendly name ("Ethernet 2"), MAC or IPv4.
    property String^ ProfinetAdapter;
    property String^ ProfinetStationName;
    /// File that keeps the NameOfStation assigned by the controller.
    property String^ ProfinetPersistPath;
    property int ProfinetVendorId;
    property int ProfinetDeviceId;
    property int ProfinetMinUpdateTimeMs;
    property bool ProfinetSendVlanTag;
    property bool ProfinetPromiscuous;
};

public ref class FieldbusInputsEventArgs sealed : EventArgs {
public:
    explicit FieldbusInputsEventArgs(array<Byte>^ data) : data_(data) {}
    /// New input area (PLC -> PC).
    property array<Byte>^ Data { array<Byte>^ get() { return data_; } }

private:
    array<Byte>^ data_;
};

public ref class FieldbusStateEventArgs sealed : EventArgs {
public:
    explicit FieldbusStateEventArgs(FieldbusState state) : state_(state) {}
    property FieldbusState State { FieldbusState get() { return state_; } }

private:
    FieldbusState state_;
};

public ref class FieldbusLogEventArgs sealed : EventArgs {
public:
    explicit FieldbusLogEventArgs(String^ message) : message_(message) {}
    property String^ Message { String^ get() { return message_; } }

private:
    String^ message_;
};

/// Software fieldbus device (slave/adapter). The PLC is the master.
///
/// Threading: events are raised on the internal network thread. Keep handlers
/// short, marshal to the UI thread yourself, and never call Stop()/Dispose()
/// from inside a handler. Exceptions thrown by handlers are reported via Log.
public ref class FieldbusDevice sealed {
public:
    explicit FieldbusDevice(FieldbusDeviceConfig^ config);
    ~FieldbusDevice(); // IDisposable.Dispose
    !FieldbusDevice(); // finalizer

    /// Opens the sockets of the selected transport. Throws InvalidOperationException on failure.
    void Start();
    void Stop();

    /// Whole input area (PLC -> PC). Zeros before Start().
    array<Byte>^ IoRead();
    /// Fills buffer from the input area starting at offset.
    void IoRead(int offset, array<Byte>^ buffer);
    /// Writes data into the output area (PC -> PLC) at offset. Can be called before Start().
    void IoWrite(int offset, array<Byte>^ data);

    property FieldbusState State { FieldbusState get(); }
    property FieldbusTransport Transport { FieldbusTransport get() { return config_->Transport; } }
    property int InputSize { int get() { return inputSize_; } }
    property int OutputSize { int get() { return outputSize_; } }
    property bool IsRunning { bool get() { return native_ != nullptr; } }

    event EventHandler<FieldbusInputsEventArgs^>^ InputsChanged;
    event EventHandler<FieldbusStateEventArgs^>^ StateChanged;
    event EventHandler<FieldbusLogEventArgs^>^ Log;

internal:
    void RaiseInputs(const std::vector<uint8_t>& data);
    void RaiseState(softfb::DeviceState state);
    void RaiseLog(const std::string& message);

private:
    void DestroyNative();

    FieldbusDeviceConfig^ config_;
    softfb::FieldbusDevice* native_; // managed fields start zeroed (no NSDMI in ref classes)
    array<Byte>^ pendingOutputs_;    // output area written before Start()
    int inputSize_;
    int outputSize_;
    Object^ startStopLock_;
};

} // namespace SoftFieldbus
```

### Key Concepts
1. **Enum values mirror the native enum** (checked by a `static_assert` in the .cpp).

---

## Step 9.17.2: `SoftFieldbusNet.cpp`

**File:** `dotnet/SoftFieldbusNet.cpp`

### Purpose
Range checks and copying of the new properties; defaults come from the native config.

### Implementation

```cpp
#include "SoftFieldbusNet.h"

#include <cstring>
#include <msclr/lock.h>
#include <msclr/marshal_cppstd.h>
#include <vcclr.h>

using namespace System;

namespace SoftFieldbus {

static_assert(int(softfb::Transport::EtherNetIP) == 0 && int(softfb::Transport::ModbusTcp) == 1 &&
                  int(softfb::Transport::ModbusUdp) == 2 && int(softfb::Transport::ModbusTcpUdp) == 3 &&
                  int(softfb::Transport::ProfinetRt) == 4,
              "FieldbusTransport must mirror softfb::Transport");
static_assert(int(softfb::DeviceState::Stopped) == 0 && int(softfb::DeviceState::WaitingForMaster) == 1 &&
                  int(softfb::DeviceState::ConnectedIdle) == 2 && int(softfb::DeviceState::ConnectedRun) == 3,
              "FieldbusState must mirror softfb::DeviceState");

namespace {

int checkRange(int value, int min, int max, String^ name)
{
    if (value < min || value > max)
        throw gcnew ArgumentOutOfRangeException(name, value,
                                                String::Format("must be between {0} and {1}", min, max));
    return value;
}

std::string toStd(String^ s) { return s == nullptr ? std::string() : msclr::interop::marshal_as<std::string>(s); }

softfb::DeviceConfig toNative(FieldbusDeviceConfig^ c)
{
    softfb::DeviceConfig n;
    n.transport = softfb::Transport(int(c->Transport));
    if (c->BindAddress != nullptr)
        n.bindAddress = toStd(c->BindAddress);
    n.raiseThreadPriority = c->RaiseThreadPriority;

    // EtherNet/IP class-1 sizes are limited to 9 bits minus headers; Modbus areas to the 16-bit address space.
    bool eip = c->Transport == FieldbusTransport::EtherNetIP;
    n.inputSize = size_t(checkRange(c->InputSize, 0, eip ? 505 : 131072, "InputSize"));
    n.outputSize = size_t(checkRange(c->OutputSize, 0, eip ? 509 : 131072, "OutputSize"));

    n.eip.plcToPcInstance = uint16_t(checkRange(c->EipPlcToPcInstance, 1, 0xFFFF, "EipPlcToPcInstance"));
    n.eip.pcToPlcInstance = uint16_t(checkRange(c->EipPcToPlcInstance, 1, 0xFFFF, "EipPcToPlcInstance"));
    n.eip.configInstance = uint16_t(checkRange(c->EipConfigInstance, 1, 0xFFFF, "EipConfigInstance"));
    n.eip.minRpiUs = uint32_t(checkRange(c->EipMinRpiMicroseconds, 0, Int32::MaxValue, "EipMinRpiMicroseconds"));
    n.eip.identity.vendorId = uint16_t(checkRange(c->VendorId, 0, 0xFFFF, "VendorId"));
    n.eip.identity.deviceType = uint16_t(checkRange(c->DeviceType, 0, 0xFFFF, "DeviceType"));
    n.eip.identity.productCode = uint16_t(checkRange(c->ProductCode, 0, 0xFFFF, "ProductCode"));
    n.eip.identity.revisionMajor = uint8_t(checkRange(c->RevisionMajor, 1, 0x7F, "RevisionMajor"));
    n.eip.identity.revisionMinor = uint8_t(checkRange(c->RevisionMinor, 0, 0xFF, "RevisionMinor"));
    n.eip.identity.serialNumber = c->SerialNumber;
    if (c->ProductName != nullptr)
        n.eip.identity.productName = toStd(c->ProductName);

    n.modbus.port = uint16_t(checkRange(c->ModbusPort, 1, 0xFFFF, "ModbusPort"));
    n.modbus.unitId = uint8_t(checkRange(c->ModbusUnitId, 0, 0xFF, "ModbusUnitId"));
    n.modbus.outputsInHoldingAt = checkRange(c->ModbusOutputsInHoldingAt, -1, 0xFFFF, "ModbusOutputsInHoldingAt");
    if (c->ModbusVendorName != nullptr)
        n.modbus.vendorName = toStd(c->ModbusVendorName);
    if (c->ModbusProductCode != nullptr)
        n.modbus.productCode = toStd(c->ModbusProductCode);
    if (c->ModbusRevision != nullptr)
        n.modbus.revision = toStd(c->ModbusRevision);

    if (c->ProfinetAdapter != nullptr)
        n.profinet.adapter = toStd(c->ProfinetAdapter);
    if (c->ProfinetStationName != nullptr)
        n.profinet.stationName = toStd(c->ProfinetStationName);
    if (c->ProfinetPersistPath != nullptr)
        n.profinet.persistPath = toStd(c->ProfinetPersistPath);
    n.profinet.vendorId = uint16_t(checkRange(c->ProfinetVendorId, 0, 0xFFFF, "ProfinetVendorId"));
    n.profinet.deviceId = uint16_t(checkRange(c->ProfinetDeviceId, 0, 0xFFFF, "ProfinetDeviceId"));
    n.profinet.minUpdateTimeMs = uint32_t(checkRange(c->ProfinetMinUpdateTimeMs, 1, 512, "ProfinetMinUpdateTimeMs"));
    n.profinet.sendVlanTag = c->ProfinetSendVlanTag;
    n.profinet.promiscuous = c->ProfinetPromiscuous;
    return n;
}

array<Byte>^ toManaged(const std::vector<uint8_t>& v)
{
    auto result = gcnew array<Byte>(int(v.size()));
    if (!v.empty()) {
        pin_ptr<Byte> p = &result[0];
        std::memcpy(p, v.data(), v.size());
    }
    return result;
}

// Native callbacks -> managed events. Lambdas are not allowed inside member
// functions of a ref class (C3923), hence this free function. gcroot holds a
// GC handle to the device for as long as the native object lives.
void installCallbacks(softfb::DeviceConfig& cfg, FieldbusDevice^ device)
{
    gcroot<FieldbusDevice^> self(device);
    cfg.onLog = [self](const std::string& msg) { self->RaiseLog(msg); };
    cfg.onInputsChanged = [self](const std::vector<uint8_t>& data) { self->RaiseInputs(data); };
    cfg.onStateChanged = [self](softfb::DeviceState s) { self->RaiseState(s); };
}

} // namespace

// ---------------------------------------------------------------------------
// FieldbusDeviceConfig
// ---------------------------------------------------------------------------

FieldbusDeviceConfig::FieldbusDeviceConfig()
{
    const softfb::DeviceConfig d; // single source of truth for defaults
    Transport = FieldbusTransport(int(d.transport));
    BindAddress = gcnew String(d.bindAddress.c_str());
    InputSize = int(d.inputSize);
    OutputSize = int(d.outputSize);
    RaiseThreadPriority = d.raiseThreadPriority;

    EipPlcToPcInstance = d.eip.plcToPcInstance;
    EipPcToPlcInstance = d.eip.pcToPlcInstance;
    EipConfigInstance = d.eip.configInstance;
    EipMinRpiMicroseconds = int(d.eip.minRpiUs);
    VendorId = d.eip.identity.vendorId;
    DeviceType = d.eip.identity.deviceType;
    ProductCode = d.eip.identity.productCode;
    RevisionMajor = d.eip.identity.revisionMajor;
    RevisionMinor = d.eip.identity.revisionMinor;
    SerialNumber = d.eip.identity.serialNumber;
    ProductName = gcnew String(d.eip.identity.productName.c_str());

    ModbusPort = d.modbus.port;
    ModbusUnitId = d.modbus.unitId;
    ModbusOutputsInHoldingAt = d.modbus.outputsInHoldingAt;
    ModbusVendorName = gcnew String(d.modbus.vendorName.c_str());
    ModbusProductCode = gcnew String(d.modbus.productCode.c_str());
    ModbusRevision = gcnew String(d.modbus.revision.c_str());

    ProfinetAdapter = gcnew String(d.profinet.adapter.c_str());
    ProfinetStationName = gcnew String(d.profinet.stationName.c_str());
    ProfinetPersistPath = gcnew String(d.profinet.persistPath.c_str());
    ProfinetVendorId = d.profinet.vendorId;
    ProfinetDeviceId = d.profinet.deviceId;
    ProfinetMinUpdateTimeMs = int(d.profinet.minUpdateTimeMs);
    ProfinetSendVlanTag = d.profinet.sendVlanTag;
    ProfinetPromiscuous = d.profinet.promiscuous;
}

// ---------------------------------------------------------------------------
// FieldbusDevice
// ---------------------------------------------------------------------------

FieldbusDevice::FieldbusDevice(FieldbusDeviceConfig^ config)
{
    if (config == nullptr)
        throw gcnew ArgumentNullException("config");
    config_ = config;
    inputSize_ = config->InputSize;
    outputSize_ = config->OutputSize;
    startStopLock_ = gcnew Object();
}

FieldbusDevice::~FieldbusDevice()
{
    Stop();
    this->!FieldbusDevice();
}

FieldbusDevice::!FieldbusDevice()
{
    // While running, the gcroot handles inside the native callbacks keep this
    // object alive, so the finalizer only ever sees a stopped device.
    DestroyNative();
}

void FieldbusDevice::DestroyNative()
{
    softfb::FieldbusDevice* n = native_;
    native_ = nullptr;
    if (n) {
        n->stop(); // joins the network thread: no callbacks after this
        delete n;
    }
}

void FieldbusDevice::Start()
{
    msclr::lock guard(startStopLock_);
    if (native_)
        return;

    softfb::DeviceConfig cfg = toNative(config_);
    inputSize_ = int(cfg.inputSize);
    outputSize_ = int(cfg.outputSize);
    installCallbacks(cfg, this);

    auto device = new softfb::FieldbusDevice(cfg);
    if (pendingOutputs_ != nullptr && pendingOutputs_->Length > 0) {
        int n = Math::Min(pendingOutputs_->Length, outputSize_);
        if (n > 0) {
            pin_ptr<Byte> p = &pendingOutputs_[0];
            device->ioWrite(0, p, size_t(n));
        }
    }
    std::string error;
    if (!device->start(&error)) {
        delete device;
        throw gcnew InvalidOperationException(gcnew String(("Fieldbus device start failed: " + error).c_str()));
    }
    native_ = device;
}

void FieldbusDevice::Stop()
{
    msclr::lock guard(startStopLock_);
    DestroyNative();
}

array<Byte>^ FieldbusDevice::IoRead()
{
    auto result = gcnew array<Byte>(inputSize_);
    IoRead(0, result);
    return result;
}

void FieldbusDevice::IoRead(int offset, array<Byte>^ buffer)
{
    if (buffer == nullptr)
        throw gcnew ArgumentNullException("buffer");
    if (offset < 0 || buffer->Length > inputSize_ - offset)
        throw gcnew ArgumentOutOfRangeException("offset", "offset + buffer length exceeds InputSize");
    if (buffer->Length == 0)
        return;
    softfb::FieldbusDevice* n = native_;
    if (!n) {
        Array::Clear(buffer, 0, buffer->Length);
        return;
    }
    pin_ptr<Byte> p = &buffer[0];
    n->ioRead(size_t(offset), p, size_t(buffer->Length));
}

void FieldbusDevice::IoWrite(int offset, array<Byte>^ data)
{
    if (data == nullptr)
        throw gcnew ArgumentNullException("data");
    if (offset < 0 || data->Length > outputSize_ - offset)
        throw gcnew ArgumentOutOfRangeException("offset", "offset + data length exceeds OutputSize");
    if (data->Length == 0)
        return;
    softfb::FieldbusDevice* n = native_;
    if (!n) {
        if (pendingOutputs_ == nullptr || pendingOutputs_->Length != outputSize_)
            pendingOutputs_ = gcnew array<Byte>(outputSize_);
        Array::Copy(data, 0, pendingOutputs_, offset, data->Length);
        return;
    }
    pin_ptr<Byte> p = &data[0];
    n->ioWrite(size_t(offset), p, size_t(data->Length));
}

FieldbusState FieldbusDevice::State::get()
{
    softfb::FieldbusDevice* n = native_;
    return n ? FieldbusState(int(n->state())) : FieldbusState::Stopped;
}

// A managed exception must never unwind into the native network thread.

void FieldbusDevice::RaiseInputs(const std::vector<uint8_t>& data)
{
    try {
        InputsChanged(this, gcnew FieldbusInputsEventArgs(toManaged(data)));
    } catch (Exception^ ex) {
        RaiseLog("InputsChanged handler threw: " + msclr::interop::marshal_as<std::string>(ex->Message));
    }
}

void FieldbusDevice::RaiseState(softfb::DeviceState state)
{
    try {
        StateChanged(this, gcnew FieldbusStateEventArgs(FieldbusState(int(state))));
    } catch (Exception^ ex) {
        RaiseLog("StateChanged handler threw: " + msclr::interop::marshal_as<std::string>(ex->Message));
    }
}

void FieldbusDevice::RaiseLog(const std::string& message)
{
    try {
        Log(this, gcnew FieldbusLogEventArgs(gcnew String(message.c_str())));
    } catch (Exception^) {
        // nothing sensible left to report to
    }
}

} // namespace SoftFieldbus
```

### Key Concepts
1. **Single source of defaults**: the managed config reads `softfb::DeviceConfig{}`.

---

## Step 9.17.3: `Program.cs`

**File:** `dotnet/fieldbus_sample/Program.cs`

### Purpose
`SoftFieldbusSample profinet "Ethernet 2" [station-name]`.

### Implementation

```csharp
// C# version of fb_device_demo: the same app on any fieldbus.
//   input area  (PLC -> PC)  printed when the PLC changes it
//   output area (PC -> PLC)  = echo of the input area, bytes 0..3 = heartbeat
// Usage: SoftFieldbusSample [eip|modbus-tcp|modbus-udp|modbus] [bind-ip]
//        SoftFieldbusSample profinet <adapter> [station-name]
using System;
using System.Threading;
using SoftFieldbus;

var transport = FieldbusTransport.EtherNetIP;
if (args.Length > 0)
{
    transport = args[0].ToLowerInvariant() switch
    {
        "eip" => FieldbusTransport.EtherNetIP,
        "modbus-tcp" => FieldbusTransport.ModbusTcp,
        "modbus-udp" => FieldbusTransport.ModbusUdp,
        "modbus" => FieldbusTransport.ModbusTcpUdp,
        "profinet" => FieldbusTransport.ProfinetRt,
        _ => throw new ArgumentException($"unknown transport {args[0]}"),
    };
}

var cfg = new FieldbusDeviceConfig
{
    Transport = transport,
    InputSize = 64,   // PLC -> PC
    OutputSize = 64,  // PC -> PLC
    ModbusUnitId = 1,
    ProductName = "SoftFieldbus C# Sample",
};
if (transport == FieldbusTransport.ProfinetRt)
{
    if (args.Length < 2)
        throw new ArgumentException("profinet needs the adapter, e.g.: SoftFieldbusSample profinet \"Ethernet 2\"");
    cfg.ProfinetAdapter = args[1];
    if (args.Length > 2)
        cfg.ProfinetStationName = args[2];
}
else if (args.Length > 1)
    cfg.BindAddress = args[1];

using var dev = new FieldbusDevice(cfg);

// Raised on the network thread. In WPF/WinUI marshal to the UI thread
// (Dispatcher.BeginInvoke / DispatcherQueue.TryEnqueue) before touching controls.
dev.Log += (_, e) => Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} {e.Message}");
dev.StateChanged += (_, e) => Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} state: {e.State}");
dev.InputsChanged += (_, e) =>
    Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} inputs (PLC -> PC) " +
                      BitConverter.ToString(e.Data, 0, Math.Min(16, e.Data.Length)));

dev.Start();
Console.WriteLine($"Transport {dev.Transport}: input {dev.InputSize} B (PLC -> PC), " +
                  $"output {dev.OutputSize} B (PC -> PLC). Ctrl+C to quit.");

using var quit = new ManualResetEventSlim();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; quit.Set(); };

// The application: identical for every transport.
var outputs = new byte[dev.OutputSize];
uint heartbeat = 0;
while (!quit.Wait(10))
{
    byte[] inputs = dev.IoRead();
    Array.Copy(inputs, outputs, Math.Min(inputs.Length, outputs.Length));
    BitConverter.GetBytes(++heartbeat).CopyTo(outputs, 0);
    dev.IoWrite(0, outputs);
}

dev.Stop();
Console.WriteLine("stopped");
```

### Key Concepts
1. **Events on the network thread**: marshal to the UI thread in real applications.

---

## Step 9.17.4: Build note

### Purpose
Command-line builds of the C# projects need a NuGet restore.

### Procedure
Visual Studio restores automatically. From the command line:
```bat
cmake --build --preset vs2026-release -- /restore
```
Deploy `SoftFieldbus.Net.dll` and `Ijwhost.dll`; Npcap must be installed on the target PC.

---

## Verification Checklist

- [ ] `SoftFieldbusSample profinet "Ethernet 2" softpn-device` reaches ConnectedRun and prints InputsChanged
- [ ] EtherNet/IP and Modbus modes of the sample unchanged

---

## Next Phase
Continue to [Phase9.18-CodesysBringUp.md](Phase9.18-CodesysBringUp.md): Windows setup and CODESYS bring-up (Gates C and D).
