// SoftEip.Net - .NET wrapper (C++/CLI) around softeip::Adapter.
//
// C# usage:
//   var cfg = new EipAdapterConfig { InputSize = 32, OutputSize = 32, BindAddress = "192.168.1.50" };
//   using var eip = new EipAdapter(cfg);
//   eip.OutputsChanged += (s, e) => ...;   // raised on the network thread!
//   eip.Start();
//   eip.SetInputs(bytes);  byte[] outs = eip.GetOutputs();
#pragma once

#include "softeip/eip_adapter.hpp"

#include <string>
#include <vector>

namespace SoftFieldbus {

using namespace System;

/// Configuration, read when Start() is called. Defaults match the Studio 5000
/// Generic Ethernet Module setup in the README (100 / 150 / 151, 32 bytes).
public ref class EipAdapterConfig sealed {
public:
    EipAdapterConfig();

    /// IP of the NIC facing the PLC, or "0.0.0.0" for all NICs.
    property String^ BindAddress;
    /// Bytes adapter -> PLC (T->O, assembly InputInstance).
    property int InputSize;
    /// Bytes PLC -> adapter (O->T, assembly OutputInstance).
    property int OutputSize;
    property int InputInstance;
    property int OutputInstance;
    property int ConfigInstance;
    property int InputOnlyInstance;
    property int ListenOnlyInstance;
    /// Forward_Open with a faster RPI is rejected.
    property int MinRpiMicroseconds;
    property bool RaiseThreadPriority;

    // Identity object
    property int VendorId;
    property int DeviceType;
    property int ProductCode;
    property int RevisionMajor;
    property int RevisionMinor;
    property UInt32 SerialNumber;
    property String^ ProductName;
};

public ref class EipOutputsEventArgs sealed : EventArgs {
public:
    EipOutputsEventArgs(array<Byte>^ data, bool plcRun) : data_(data), plcRun_(plcRun) {}
    /// New output image written by the PLC.
    property array<Byte>^ Data { array<Byte>^ get() { return data_; } }
    /// Run/idle bit: false when the PLC is in program mode or the connection was lost.
    property bool PlcRun { bool get() { return plcRun_; } }

private:
    array<Byte>^ data_;
    bool plcRun_;
};

public ref class EipConnectionEventArgs sealed : EventArgs {
public:
    explicit EipConnectionEventArgs(bool connected) : connected_(connected) {}
    property bool Connected { bool get() { return connected_; } }

private:
    bool connected_;
};

public ref class EipLogEventArgs sealed : EventArgs {
public:
    explicit EipLogEventArgs(String^ message) : message_(message) {}
    property String^ Message { String^ get() { return message_; } }

private:
    String^ message_;
};

/// EtherNet/IP adapter (I/O device). The PLC is the scanner/master.
///
/// Threading: events are raised on the internal network thread. Keep handlers
/// short, marshal to the UI thread yourself (Dispatcher / DispatcherQueue), and
/// never call Stop()/Dispose() from inside a handler. Exceptions thrown by
/// handlers are caught and reported through the Log event.
public ref class EipAdapter sealed {
public:
    explicit EipAdapter(EipAdapterConfig^ config);
    ~EipAdapter();  // IDisposable.Dispose
    !EipAdapter();  // finalizer

    /// Opens TCP/UDP 44818 and UDP 2222 and starts the network thread.
    /// Throws InvalidOperationException (e.g. port already in use).
    void Start();
    /// Closes all connections and sockets. Safe to call more than once.
    void Stop();

    /// Sets the input image the PLC reads. Can be called before Start().
    void SetInputs(array<Byte>^ data);
    /// Returns a copy of the last output image written by the PLC.
    array<Byte>^ GetOutputs();

    property bool IsRunning { bool get() { return native_ != nullptr; } }
    property bool PlcInRun { bool get(); }
    property bool OutputConnected { bool get(); }

    event EventHandler<EipOutputsEventArgs^>^ OutputsChanged;
    event EventHandler<EipConnectionEventArgs^>^ ConnectionChanged;
    event EventHandler<EipLogEventArgs^>^ Log;

internal:
    void RaiseOutputs(const std::vector<uint8_t>& data, bool run);
    void RaiseConnection(bool connected);
    void RaiseLog(const std::string& message);

private:
    void DestroyNative();

    EipAdapterConfig^ config_;
    softeip::Adapter* native_; // managed fields start zeroed (no NSDMI in ref classes)
    array<Byte>^ pendingInputs_;
    Object^ startStopLock_;
};

} // namespace SoftFieldbus
