// SoftFieldbus.Net - .NET wrapper (C++/CLI) around softfb::FieldbusDevice.
// One Hilscher-style API for every transport (EtherNet/IP, Modbus TCP/UDP).
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
