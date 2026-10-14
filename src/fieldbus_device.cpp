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
