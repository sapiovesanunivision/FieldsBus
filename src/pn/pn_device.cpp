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
                const LinkStatus ls = port->link(); // every 5 s: follows a renegotiated link speed
                if (const uint16_t mau = lldp::mauTypeFor(ls.speedMbps, ls.fullDuplex))
                    li.mauType = mau;
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
        opt.backend = c.l2Backend;
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
    ac.controllerRpcPort = c.controllerRpcPort;
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
    // Bound to the NIC's address: the replies leave through this NIC with this source IP even when the PC has
    // another interface in the same subnet (e.g. a second NIC for the engineering tool).
    if (!d.rpc.open(c.rpcPort, &err, nic.ipv4)) {
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
