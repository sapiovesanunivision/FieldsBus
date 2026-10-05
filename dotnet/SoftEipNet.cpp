#include "SoftEipNet.h"

#include <cstring>
#include <msclr/lock.h>
#include <msclr/marshal_cppstd.h>
#include <vcclr.h>

using namespace System;

namespace SoftFieldbus {

namespace {

int checkRange(int value, int min, int max, String^ name)
{
    if (value < min || value > max)
        throw gcnew ArgumentOutOfRangeException(name, value,
                                                String::Format("must be between {0} and {1}", min, max));
    return value;
}

softeip::AdapterConfig toNative(EipAdapterConfig^ c)
{
    softeip::AdapterConfig n;
    if (c->BindAddress != nullptr)
        n.bindAddress = msclr::interop::marshal_as<std::string>(c->BindAddress);
    // Sizes: class-1 size field is 9 bits, minus sequence count / run-idle header.
    n.inputSize = size_t(checkRange(c->InputSize, 0, 509, "InputSize"));
    n.outputSize = size_t(checkRange(c->OutputSize, 0, 505, "OutputSize"));
    n.inputInstance = uint16_t(checkRange(c->InputInstance, 1, 0xFFFF, "InputInstance"));
    n.outputInstance = uint16_t(checkRange(c->OutputInstance, 1, 0xFFFF, "OutputInstance"));
    n.configInstance = uint16_t(checkRange(c->ConfigInstance, 1, 0xFFFF, "ConfigInstance"));
    n.inputOnlyInstance = uint16_t(checkRange(c->InputOnlyInstance, 1, 0xFFFF, "InputOnlyInstance"));
    n.listenOnlyInstance = uint16_t(checkRange(c->ListenOnlyInstance, 1, 0xFFFF, "ListenOnlyInstance"));
    n.minRpiUs = uint32_t(checkRange(c->MinRpiMicroseconds, 0, Int32::MaxValue, "MinRpiMicroseconds"));
    n.raiseThreadPriority = c->RaiseThreadPriority;

    n.identity.vendorId = uint16_t(checkRange(c->VendorId, 0, 0xFFFF, "VendorId"));
    n.identity.deviceType = uint16_t(checkRange(c->DeviceType, 0, 0xFFFF, "DeviceType"));
    n.identity.productCode = uint16_t(checkRange(c->ProductCode, 0, 0xFFFF, "ProductCode"));
    n.identity.revisionMajor = uint8_t(checkRange(c->RevisionMajor, 1, 0x7F, "RevisionMajor"));
    n.identity.revisionMinor = uint8_t(checkRange(c->RevisionMinor, 0, 0xFF, "RevisionMinor"));
    n.identity.serialNumber = c->SerialNumber;
    if (c->ProductName != nullptr)
        n.identity.productName = msclr::interop::marshal_as<std::string>(c->ProductName);
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
// GC handle to the adapter for as long as the native Adapter lives.
void installCallbacks(softeip::AdapterConfig& cfg, EipAdapter^ adapter)
{
    gcroot<EipAdapter^> self(adapter);
    cfg.onLog = [self](const std::string& msg) { self->RaiseLog(msg); };
    cfg.onOutputs = [self](const std::vector<uint8_t>& data, bool run) { self->RaiseOutputs(data, run); };
    cfg.onConnectionChanged = [self](bool connected) { self->RaiseConnection(connected); };
}

} // namespace

// ---------------------------------------------------------------------------
// EipAdapterConfig
// ---------------------------------------------------------------------------

EipAdapterConfig::EipAdapterConfig()
{
    const softeip::AdapterConfig d; // single source of truth for defaults
    BindAddress = gcnew String(d.bindAddress.c_str());
    InputSize = int(d.inputSize);
    OutputSize = int(d.outputSize);
    InputInstance = d.inputInstance;
    OutputInstance = d.outputInstance;
    ConfigInstance = d.configInstance;
    InputOnlyInstance = d.inputOnlyInstance;
    ListenOnlyInstance = d.listenOnlyInstance;
    MinRpiMicroseconds = int(d.minRpiUs);
    RaiseThreadPriority = d.raiseThreadPriority;
    VendorId = d.identity.vendorId;
    DeviceType = d.identity.deviceType;
    ProductCode = d.identity.productCode;
    RevisionMajor = d.identity.revisionMajor;
    RevisionMinor = d.identity.revisionMinor;
    SerialNumber = d.identity.serialNumber;
    ProductName = gcnew String(d.identity.productName.c_str());
}

// ---------------------------------------------------------------------------
// EipAdapter
// ---------------------------------------------------------------------------

EipAdapter::EipAdapter(EipAdapterConfig^ config)
{
    if (config == nullptr)
        throw gcnew ArgumentNullException("config");
    config_ = config;
    startStopLock_ = gcnew Object();
}

EipAdapter::~EipAdapter()
{
    Stop();
    this->!EipAdapter();
}

EipAdapter::!EipAdapter()
{
    // While running, the gcroot handles inside the native callbacks keep this
    // object alive, so the finalizer only ever sees a stopped adapter. This is
    // a safety net only - always Dispose() / Stop().
    DestroyNative();
}

void EipAdapter::DestroyNative()
{
    softeip::Adapter* n = native_;
    native_ = nullptr;
    if (n) {
        n->stop(); // joins the network thread: no callbacks after this
        delete n;
    }
}

void EipAdapter::Start()
{
    msclr::lock guard(startStopLock_);
    if (native_)
        return;

    softeip::AdapterConfig cfg = toNative(config_);

    installCallbacks(cfg, this);

    auto adapter = new softeip::Adapter(cfg);
    std::string error;
    if (!adapter->start(&error)) {
        delete adapter;
        throw gcnew InvalidOperationException(gcnew String(("EtherNet/IP adapter start failed: " + error).c_str()));
    }
    native_ = adapter;

    if (pendingInputs_ != nullptr) {
        SetInputs(pendingInputs_);
        pendingInputs_ = nullptr;
    }
}

void EipAdapter::Stop()
{
    msclr::lock guard(startStopLock_);
    DestroyNative();
}

void EipAdapter::SetInputs(array<Byte>^ data)
{
    if (data == nullptr)
        throw gcnew ArgumentNullException("data");
    softeip::Adapter* n = native_;
    if (!n) {
        pendingInputs_ = safe_cast<array<Byte>^>(data->Clone());
        return;
    }
    if (data->Length == 0)
        return;
    pin_ptr<Byte> p = &data[0];
    n->setInputData(p, size_t(data->Length));
}

array<Byte>^ EipAdapter::GetOutputs()
{
    softeip::Adapter* n = native_;
    if (!n)
        return gcnew array<Byte>(config_->OutputSize);
    return toManaged(n->outputData());
}

bool EipAdapter::PlcInRun::get()
{
    softeip::Adapter* n = native_;
    return n && n->plcInRun();
}

bool EipAdapter::OutputConnected::get()
{
    softeip::Adapter* n = native_;
    return n && n->outputConnected();
}

// A managed exception must never unwind into the native network thread.

void EipAdapter::RaiseOutputs(const std::vector<uint8_t>& data, bool run)
{
    try {
        OutputsChanged(this, gcnew EipOutputsEventArgs(toManaged(data), run));
    } catch (Exception^ ex) {
        RaiseLog("OutputsChanged handler threw: " + msclr::interop::marshal_as<std::string>(ex->Message));
    }
}

void EipAdapter::RaiseConnection(bool connected)
{
    try {
        ConnectionChanged(this, gcnew EipConnectionEventArgs(connected));
    } catch (Exception^ ex) {
        RaiseLog("ConnectionChanged handler threw: " + msclr::interop::marshal_as<std::string>(ex->Message));
    }
}

void EipAdapter::RaiseLog(const std::string& message)
{
    try {
        Log(this, gcnew EipLogEventArgs(gcnew String(message.c_str())));
    } catch (Exception^) {
        // nothing sensible left to report to
    }
}

} // namespace SoftFieldbus
