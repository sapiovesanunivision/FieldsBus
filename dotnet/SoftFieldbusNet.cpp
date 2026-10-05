#include "SoftFieldbusNet.h"

#include <cstring>
#include <msclr/lock.h>
#include <msclr/marshal_cppstd.h>
#include <vcclr.h>

using namespace System;

namespace SoftFieldbus {

static_assert(int(softfb::Transport::EtherNetIP) == 0 && int(softfb::Transport::ModbusTcp) == 1 &&
                  int(softfb::Transport::ModbusUdp) == 2 && int(softfb::Transport::ModbusTcpUdp) == 3,
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
