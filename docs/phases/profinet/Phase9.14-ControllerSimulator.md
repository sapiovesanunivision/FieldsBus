# Phase 9.14: Controller simulator (pn_controller_sim)

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
A minimal IO-Controller that runs the same startup as a PLC, to test the device without CODESYS: DCP Identify, Connect, Write, Read I&M0, PrmEnd + ApplicationReady, cyclic echo, Release, and error cases.

## Documentation References
- [Phase 9 spec](../../profinet-spec.md) phase 9f
- [ip-netns(8)](https://man7.org/linux/man-pages/man8/ip-netns.8.html)

---

## Step 9.14.1: `pn_controller.hpp`

**File:** `tools/pn_controller.hpp`

### Purpose
Reusable class (also used by `pn_selftest`), one method per startup step.

### Implementation

```cpp
// Minimal PROFINET IO-Controller, used to test softpn::Device without a PLC
// (pn_controller_sim on a NIC, pn_selftest over an in-memory cable).
//
// It does what a PLC does at startup, one step per method:
//   identify()   DCP Identify by NameOfStation -> device MAC and IP
//   setName()    DCP Set NameOfStation (optional)
//   connect()    RPC Connect with AR, 2 IOCRs, AlarmCR and the expected submodules
//   writeRecords(), readIm0()
//   prmEnd()     RPC Control(PrmEnd), then waits for the device's ApplicationReady and confirms it
//   exchange()   cyclic output frames + check that the device echoes them
//   release()    RPC Release
// It is a test tool: one AR, fixed slot layout matching the softpn GSDML, no PDev checks.
#pragma once

#include "pn_blocks.hpp"
#include "pn_frame.hpp"
#include "pn_rpc.hpp"
#include "softpn/l2_port.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace softpn {

struct ControllerConfig {
    std::shared_ptr<L2Port> port;          // NIC or one end of makeMemoryCable()
    std::string deviceName = "softpn-device";
    uint16_t localRpcPort = 34965;         // our RPC port (the device sends ApplicationReady there)
    uint16_t deviceRpcPort = 34964;
    uint16_t vendorId = 0xFFFF;
    uint16_t deviceId = 0x0001;
    size_t inputSize = 32;                 // device -> controller (slot 1)
    size_t outputSize = 32;                // controller -> device (slot 2)
    uint16_t reductionRatio = 8;           // update time = RR ms (SendClockFactor 32)
    uint16_t dataHoldFactor = 3;
    bool advancedStartup = false;          // ARProperties.StartupMode
    bool wrongModule = false;              // expect a wrong ident in slot 1 -> ModuleDiffBlock
    bool vlan = true;                      // output frames with 802.1Q priority tag (prio 6, VID 0)
    std::function<void(const std::string&)> log;
};

struct DeviceFound {
    MacAddress mac;
    uint32_t ip = 0;
    std::string name;
    uint16_t vendorId = 0;
    uint16_t deviceId = 0;
};

class ControllerSim {
public:
    explicit ControllerSim(ControllerConfig cfg);
    ~ControllerSim();
    ControllerSim(const ControllerSim&) = delete;
    ControllerSim& operator=(const ControllerSim&) = delete;

    bool start(std::string* error = nullptr); // opens the RPC port, starts the RX thread
    void stop();

    bool identify(int timeoutMs = 2000);
    bool setName(const MacAddress& mac, const std::string& name, bool permanent = true, int timeoutMs = 1000);
    // Returns the Connect PNIOStatus (ok() = AR established).
    PnioStatus connect(int timeoutMs = 2000);
    bool writeRecords(int timeoutMs = 1000);
    bool readIm0(Im0Data& out, int timeoutMs = 1000);
    bool prmEnd(int timeoutMs = 3000); // includes waiting for and confirming ApplicationReady
    // Sends output frames for `ms` milliseconds (changing the data every 50 ms) and counts echoes.
    void exchange(int ms);
    void stopCyclic() { cyclicOn_ = false; } // simulates a dead controller: the device watchdog fires
    bool release(int timeoutMs = 1000);
    // Second, independent AR on the same device (must be refused while one is active).
    PnioStatus connectSecondAr(int timeoutMs = 2000);

    const DeviceFound& device() const { return device_; }
    const std::vector<DiffModule>& moduleDiff() const { return diff_; }
    bool arServerBlockSeen() const { return arServerBlock_; }
    uint64_t framesSent() const { return framesSent_; }
    uint64_t framesReceived() const { return framesReceived_; }
    uint64_t echoMatches() const { return echoMatches_; }
    bool inputIopsGood() const { return inputIopsGood_; }
    bool rtaErrorReceived() const { return rtaError_; }

private:
    using Clock = std::chrono::steady_clock;

    void log(const std::string& m) const
    {
        if (cfg_.log)
            cfg_.log(m);
    }
    void rxLoop();
    void rpcLoop();
    void cyclicLoop();
    // Sends an RPC request to the device and waits for the response blocks.
    bool call(uint16_t opnum, const std::vector<uint8_t>& blocks, NdrArgs& res, int timeoutMs);
    std::vector<uint8_t> buildConnect(const Uuid& arUuid, uint16_t sessionKey);
    std::vector<uint8_t> buildOutputFrame();

    ControllerConfig cfg_;
    std::atomic<bool> running_{false};
    std::thread rxThread_, rpcThread_, cyclicThread_;
    RpcEndpoint rpc_;
    Uuid activity_ = Uuid::random();
    uint32_t seq_ = 0;

    std::mutex m_;
    std::condition_variable cv_;
    std::map<uint32_t, std::pair<RpcHeader, std::vector<uint8_t>>> responses_; // by seqnum
    uint32_t identifyXid_ = 0;
    bool identified_ = false;
    bool setDone_ = false;
    uint8_t setError_ = 0xFF;
    bool appReadyReceived_ = false;

    DeviceFound device_;
    Uuid arUuid_;
    uint16_t sessionKey_ = 1;
    std::vector<DiffModule> diff_;
    bool arServerBlock_ = false;

    // Cyclic
    std::atomic<bool> cyclicOn_{false};
    uint16_t inputFrameId_ = 0x8001;
    uint16_t outputFrameId_ = 0x8000;
    uint16_t inputCsdu_ = 0, outputCsdu_ = 0;
    uint16_t inDataOffset_ = 0, inIopsOffset_ = 0;
    uint16_t cycleCounter_ = 0;
    std::vector<uint8_t> outPattern_, prevPattern_;
    std::atomic<uint64_t> framesSent_{0}, framesReceived_{0}, echoMatches_{0};
    std::atomic<bool> inputIopsGood_{false};
    std::atomic<bool> rtaError_{false};
};

} // namespace softpn
```

### Key Concepts
1. **Fixed layout** matching the GSDML: slot 1 input, slot 2 output, DAP submodules.
2. **Options**: wrong module, update time (RR), data-hold factor, VLAN, advanced startup.

---

## Step 9.14.2: `pn_controller.cpp`

**File:** `tools/pn_controller.cpp`

### Purpose
Builds the Connect request (AR, 2 IOCRs with frame offsets, AlarmCR, expected submodules), serves the device's ApplicationReady on its own RPC port, sends output frames with the same DeadlineTimer scheduling as the device, and counts echoes.

### Implementation

```cpp
#include "pn_controller.hpp"

#include "softeip/periodic_timer.hpp"

#include "pn_dcp.hpp"
#include "pn_defs.hpp"
#include "pn_model.hpp"

#include <algorithm>
#include <cstring>

namespace softpn {

namespace {

constexpr uint16_t kControllerAlarmRef = 0x0005;

} // namespace

ControllerSim::ControllerSim(ControllerConfig cfg) : cfg_(std::move(cfg)) {}

ControllerSim::~ControllerSim() { stop(); }

bool ControllerSim::start(std::string* error)
{
    if (!cfg_.port) {
        if (error)
            *error = "no L2 port";
        return false;
    }
    if (!rpc_.open(cfg_.localRpcPort, error))
        return false;
    rpc_.setResponseHandler([this](const RpcHeader& h, const std::vector<uint8_t>& body, const UdpAddress&) {
        std::lock_guard<std::mutex> lock(m_);
        responses_[h.seqnum] = {h, body};
        cv_.notify_all();
    });
    // The device calls us: ApplicationReady (Control on the controller interface).
    rpc_.setRequestHandler([this](const RpcRequest& req, std::vector<uint8_t>& res) {
        if (req.header.interfaceId != pnioControllerInterface() || req.header.opnum != rpc::kOpControl)
            return false;
        try {
            NdrArgs a = parseNdrRequest(req.body.data(), req.body.size(), req.header.littleEndian);
            size_t used = 0;
            ControlBlock cb = parseControlBlock(readBlock(a.blocks.data(), a.blocks.size(), used));
            if (cb.type != block::kApplicationReadyReq || !(cb.command & kControlApplicationReady))
                return false;
            ControlBlock done = cb;
            done.type = block::kApplicationReadyRes;
            done.command = kControlDone;
            ByteWriter w;
            writeControlBlock(w, done);
            res = buildNdrResponse(PnioStatus{}, a.argsMaximum, w.data());
            std::lock_guard<std::mutex> lock(m_);
            appReadyReceived_ = true;
            cv_.notify_all();
            return true;
        } catch (const ParseError&) {
            return false;
        }
    });
    running_ = true;
    rxThread_ = std::thread([this] { rxLoop(); });
    rpcThread_ = std::thread([this] { rpcLoop(); });
    cyclicThread_ = std::thread([this] { cyclicLoop(); });
    return true;
}

void ControllerSim::stop()
{
    if (!running_)
        return;
    cyclicOn_ = false;
    running_ = false;
    cfg_.port->interrupt();
    for (std::thread* t : {&rxThread_, &rpcThread_, &cyclicThread_})
        if (t->joinable())
            t->join();
    rpc_.close();
}

void ControllerSim::rpcLoop()
{
    while (running_)
        rpc_.poll(10);
}

void ControllerSim::rxLoop()
{
    std::vector<uint8_t> f;
    while (running_) {
        if (!cfg_.port->receive(f, 50))
            continue;
        EthHeader eth;
        if (!parseEthHeader(f.data(), f.size(), eth) || eth.etherType != kEtherTypeProfinet)
            continue;
        const uint16_t id = frameIdOf(f.data(), f.size(), eth);
        if (id == kFrameIdDcpIdentifyRes || id == kFrameIdDcpGetSet) {
            DcpPdu pdu;
            try {
                pdu = parseDcp(f.data() + eth.payloadOffset, f.size() - eth.payloadOffset);
            } catch (const ParseError&) {
                continue;
            }
            std::lock_guard<std::mutex> lock(m_);
            if (pdu.xid != identifyXid_ || pdu.serviceType != dcp::kTypeResponseSuccess)
                continue;
            if (pdu.serviceId == dcp::kServiceIdentify) {
                device_.mac = eth.src;
                for (const DcpBlock& b : pdu.blocks) {
                    const std::vector<uint8_t> p = dcpPayload(b);
                    if (b.option == dcp::kOptDeviceProperties && b.suboption == dcp::kSubDevNameOfStation)
                        device_.name.assign(p.begin(), p.end());
                    else if (b.option == dcp::kOptIp && b.suboption == dcp::kSubIpParameter && p.size() >= 4)
                        device_.ip = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
                    else if (b.option == dcp::kOptDeviceProperties && b.suboption == dcp::kSubDevId && p.size() >= 4) {
                        device_.vendorId = uint16_t((p[0] << 8) | p[1]);
                        device_.deviceId = uint16_t((p[2] << 8) | p[3]);
                    }
                }
                identified_ = true;
            } else if (pdu.serviceId == dcp::kServiceSet) {
                setError_ = 0;
                for (const DcpBlock& b : pdu.blocks)
                    if (b.option == dcp::kOptControl && b.suboption == dcp::kSubCtrlResponse && b.data.size() >= 3)
                        setError_ = std::max(setError_, b.data[2]);
                setDone_ = true;
            }
            cv_.notify_all();
        } else if (id == inputFrameId_ && eth.src == device_.mac) {
            const size_t csdu = eth.payloadOffset + 2;
            if (f.size() < csdu + inputCsdu_ + 4)
                continue;
            ++framesReceived_;
            const uint8_t* p = f.data() + csdu;
            inputIopsGood_ = (p[inIopsOffset_] & kIoxsGood) != 0;
            std::lock_guard<std::mutex> lock(m_);
            // The device application echoes the outputs; it may lag a cycle behind a change.
            const size_t n = std::min(cfg_.inputSize, outPattern_.size());
            if (n > 4 && (std::memcmp(p + inDataOffset_ + 4, outPattern_.data() + 4, n - 4) == 0 ||
                          (!prevPattern_.empty() &&
                           std::memcmp(p + inDataOffset_ + 4, prevPattern_.data() + 4, n - 4) == 0)))
                ++echoMatches_;
        } else if ((id == kFrameIdAlarmLow || id == kFrameIdAlarmHigh) && eth.src == device_.mac) {
            const size_t o = eth.payloadOffset + 2;
            if (f.size() >= o + 5 && (f[o + 4] & 0x0F) == 0x04)
                rtaError_ = true;
        }
    }
}

bool ControllerSim::identify(int timeoutMs)
{
    DcpPdu req;
    req.frameId = kFrameIdDcpIdentifyReq;
    req.serviceId = dcp::kServiceIdentify;
    req.serviceType = dcp::kTypeRequest;
    req.responseDelay = 1;
    DcpBlock nameFilter;
    nameFilter.option = dcp::kOptDeviceProperties;
    nameFilter.suboption = dcp::kSubDevNameOfStation;
    nameFilter.data.assign(cfg_.deviceName.begin(), cfg_.deviceName.end());
    req.blocks.push_back(nameFilter);
    std::unique_lock<std::mutex> lock(m_);
    identified_ = false;
    req.xid = identifyXid_ = uint32_t(Clock::now().time_since_epoch().count());
    lock.unlock();
    cfg_.port->send(buildDcpFrame(MacAddress::fromBytes(dcp::kMulticastIdentify), cfg_.port->info().mac, req));
    lock.lock();
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return identified_; })) {
        log("DCP: no answer to Identify '" + cfg_.deviceName + "'");
        return false;
    }
    log("DCP: found '" + device_.name + "' at " + device_.mac.str() + ", " + ipToString(device_.ip));
    return true;
}

bool ControllerSim::setName(const MacAddress& mac, const std::string& name, bool permanent, int timeoutMs)
{
    DcpPdu req;
    req.frameId = kFrameIdDcpGetSet;
    req.serviceId = dcp::kServiceSet;
    req.serviceType = dcp::kTypeRequest;
    req.blocks.push_back(makeDcpBlock(dcp::kOptDeviceProperties, dcp::kSubDevNameOfStation,
                                      permanent ? dcp::kQualifierPermanent : dcp::kQualifierTemporary, name));
    std::unique_lock<std::mutex> lock(m_);
    setDone_ = false;
    req.xid = identifyXid_ = uint32_t(Clock::now().time_since_epoch().count());
    lock.unlock();
    cfg_.port->send(buildDcpFrame(mac, cfg_.port->info().mac, req));
    lock.lock();
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return setDone_; }))
        return false;
    return setError_ == 0;
}

bool ControllerSim::call(uint16_t opnum, const std::vector<uint8_t>& blocks, NdrArgs& res, int timeoutMs)
{
    uint32_t seq;
    {
        std::lock_guard<std::mutex> lock(m_);
        seq = seq_++;
    }
    const std::vector<uint8_t> body = buildNdrRequest(4096, blocks);
    rpc_.sendRequest(UdpAddress{device_.ip, cfg_.deviceRpcPort}, pnioDeviceObject(1, cfg_.deviceId, cfg_.vendorId),
                     pnioDeviceInterface(), opnum, activity_, seq, body);
    std::unique_lock<std::mutex> lock(m_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return responses_.count(seq) != 0; })) {
        log("RPC: no response to opnum " + std::to_string(opnum));
        return false;
    }
    auto r = std::move(responses_[seq]);
    responses_.erase(seq);
    lock.unlock();
    if (r.first.ptype != rpc::kPtypeResponse) {
        log("RPC: opnum " + std::to_string(opnum) + " rejected (ptype " + std::to_string(r.first.ptype) + ")");
        return false;
    }
    try {
        res = parseNdrResponse(r.second.data(), r.second.size(), r.first.littleEndian);
    } catch (const ParseError&) {
        return false;
    }
    return true;
}

std::vector<uint8_t> ControllerSim::buildConnect(const Uuid& arUuid, uint16_t sessionKey)
{
    const MacAddress ourMac = cfg_.port->info().mac;
    ByteWriter w;

    ArBlockReq ar;
    ar.arType = kArTypeIocarSingle;
    ar.arUuid = arUuid;
    ar.sessionKey = sessionKey;
    ar.cmInitiatorMac = ourMac;
    ar.cmInitiatorObjectUuid = pnioDeviceObject(1, 0x0002, 0x0001); // any object UUID of ours
    ar.arProperties = 0x00000001 | (cfg_.advancedStartup ? kArPropStartupAdvanced : 0);
    ar.activityTimeoutFactor = 100; // 10 s
    ar.cmInitiatorUdpPort = cfg_.localRpcPort;
    ar.cmInitiatorStationName = "softpn-controller-sim";
    writeArBlockReq(w, ar);

    const uint16_t in = uint16_t(cfg_.inputSize), out = uint16_t(cfg_.outputSize);
    auto base = [&](uint16_t type, uint16_t ref, uint16_t frameId, uint16_t len) {
        IocrBlockReq cr;
        cr.iocrType = type;
        cr.iocrReference = ref;
        cr.lt = kEtherTypeProfinet;
        cr.iocrProperties = 2; // RT_CLASS_1
        cr.dataLength = std::max<uint16_t>(len, kMinCsduLength);
        cr.frameId = frameId;
        cr.sendClockFactor = kSendClockFactor1ms;
        cr.reductionRatio = cfg_.reductionRatio;
        cr.phase = 1;
        cr.frameSendOffset = 0xFFFFFFFF;
        cr.watchdogFactor = cfg_.dataHoldFactor;
        cr.dataHoldFactor = cfg_.dataHoldFactor;
        cr.tagHeader = kVlanTciRt;
        return cr;
    };
    // Input CR C_SDU: IOPS DAP(0) IF(1) PORT(2) | slot 1 data + IOPS | IOCS of slot 2
    IocrBlockReq icr = base(kIocrTypeInput, 1, inputFrameId_, uint16_t(5 + in));
    IocrApi ia;
    ia.ioData = {{0, kSubslotDap, 0}, {0, kSubslotInterface, 1}, {0, kSubslotPort1, 2}, {kSlotInput, 1, 3}};
    ia.iocs = {{kSlotOutput, 1, uint16_t(4 + in)}};
    icr.apis.push_back(ia);
    inputCsdu_ = icr.dataLength;
    inDataOffset_ = 3;
    inIopsOffset_ = uint16_t(3 + in);
    writeIocrBlockReq(w, icr);
    // Output CR C_SDU: slot 2 data + IOPS | IOCS DAP IF PORT slot1
    IocrBlockReq ocr = base(kIocrTypeOutput, 2, outputFrameId_, uint16_t(out + 5));
    IocrApi oa;
    oa.ioData = {{kSlotOutput, 1, 0}};
    oa.iocs = {{0, kSubslotDap, uint16_t(out + 1)},
               {0, kSubslotInterface, uint16_t(out + 2)},
               {0, kSubslotPort1, uint16_t(out + 3)},
               {kSlotInput, 1, uint16_t(out + 4)}};
    ocr.apis.push_back(oa);
    outputCsdu_ = ocr.dataLength;
    writeIocrBlockReq(w, ocr);

    AlarmCrBlockReq alarm;
    alarm.alarmCrType = kAlarmCrTypeAlarm;
    alarm.lt = kEtherTypeProfinet;
    alarm.rtaTimeoutFactor = 1;
    alarm.rtaRetries = 3;
    alarm.localAlarmReference = kControllerAlarmRef;
    alarm.maxAlarmDataLength = 200;
    alarm.tagHeaderHigh = 0xC000;
    alarm.tagHeaderLow = 0xA000;
    writeAlarmCrBlockReq(w, alarm);

    const ModuleVariant* vi = findModuleVariant(cfg_.inputSize);
    const ModuleVariant* vo = findModuleVariant(cfg_.outputSize);
    ExpectedModule dap{0, 0, kDapModuleIdent, 0, {}};
    dap.submodules = {{kSubslotDap, kDapSubmoduleIdent, 0, {{1, 0, 1, 1}}},
                      {kSubslotInterface, kInterfaceSubmoduleIdent, 0, {{1, 0, 1, 1}}},
                      {kSubslotPort1, kPortSubmoduleIdent, 0, {{1, 0, 1, 1}}}};
    writeExpectedSubmoduleBlockReq(w, dap);
    ExpectedModule mi{0, kSlotInput, (vi ? vi->inputModuleIdent : 0) + (cfg_.wrongModule ? 0x100u : 0u), 0, {}};
    mi.submodules = {{1, kIoSubmoduleIdent, 1, {{1, in, 1, 1}}}};
    writeExpectedSubmoduleBlockReq(w, mi);
    ExpectedModule mo{0, kSlotOutput, vo ? vo->outputModuleIdent : 0, 0, {}};
    mo.submodules = {{1, kIoSubmoduleIdent, 2, {{2, out, 1, 1}}}};
    writeExpectedSubmoduleBlockReq(w, mo);
    return std::move(w.data());
}

PnioStatus ControllerSim::connect(int timeoutMs)
{
    arUuid_ = Uuid::random();
    NdrArgs res;
    if (!call(rpc::kOpConnect, buildConnect(arUuid_, sessionKey_), res, timeoutMs))
        return PnioStatus::make(0xFF, 0xFF, 0xFF, 0xFF);
    if (!res.status.ok()) {
        log("Connect refused: " + res.status.str());
        return res.status;
    }
    diff_.clear();
    arServerBlock_ = false;
    try {
        for (const RawBlock& b : splitBlocks(res.blocks.data(), res.blocks.size())) {
            if (b.type == block::kModuleDiffBlock)
                diff_ = parseModuleDiffBlock(b);
            else if (b.type == block::kArServerBlock)
                arServerBlock_ = true;
            else if (b.type == block::kIocrBlockRes) {
                IocrBlockRes r = parseIocrBlockRes(b);
                (r.iocrType == kIocrTypeInput ? inputFrameId_ : outputFrameId_) = r.frameId;
            }
        }
    } catch (const ParseError&) {
        log("Connect response malformed");
        return PnioStatus::make(0xFF, 0xFF, 0xFF, 0xFE);
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        outPattern_.assign(cfg_.outputSize, 0);
        prevPattern_.clear();
        appReadyReceived_ = false;
    }
    cyclicOn_ = true; // a PLC starts its output CR right after the Connect response
    log("Connect OK" + std::string(diff_.empty() ? "" : " (with ModuleDiffBlock)"));
    return res.status;
}

PnioStatus ControllerSim::connectSecondAr(int timeoutMs)
{
    NdrArgs res;
    if (!call(rpc::kOpConnect, buildConnect(Uuid::random(), uint16_t(sessionKey_ + 1)), res, timeoutMs))
        return PnioStatus::make(0xFF, 0xFF, 0xFF, 0xFF);
    return res.status;
}

bool ControllerSim::writeRecords(int timeoutMs)
{
    RecordHeader h;
    h.type = block::kWriteReqHeader;
    h.seqNumber = 1;
    h.arUuid = arUuid_;
    h.slot = 0;
    h.subslot = kSubslotPort1;
    h.index = 0x802F; // PDPortDataAdjust (content not evaluated by the device)
    const std::vector<uint8_t> data(8, 0);
    h.recordDataLength = uint32_t(data.size());
    ByteWriter w;
    writeRecordHeader(w, h);
    w.bytes(data);
    NdrArgs res;
    if (!call(rpc::kOpWrite, w.data(), res, timeoutMs))
        return false;
    if (!res.status.ok())
        log("Write refused: " + res.status.str());
    return res.status.ok();
}

bool ControllerSim::readIm0(Im0Data& out, int timeoutMs)
{
    RecordHeader h;
    h.type = block::kReadReqHeader;
    h.seqNumber = 2;
    h.arUuid = arUuid_;
    h.slot = 0;
    h.subslot = kSubslotDap;
    h.index = kIndexIm0;
    h.recordDataLength = 4096;
    ByteWriter w;
    writeRecordHeader(w, h);
    NdrArgs res;
    if (!call(rpc::kOpRead, w.data(), res, timeoutMs) || !res.status.ok())
        return false;
    try {
        size_t used = 0;
        readBlock(res.blocks.data(), res.blocks.size(), used); // IODReadResHeader
        size_t used2 = 0;
        out = parseIm0Block(readBlock(res.blocks.data() + used, res.blocks.size() - used, used2));
    } catch (const ParseError&) {
        return false;
    }
    return true;
}

bool ControllerSim::prmEnd(int timeoutMs)
{
    ControlBlock cb;
    cb.type = block::kPrmEndReq;
    cb.arUuid = arUuid_;
    cb.sessionKey = sessionKey_;
    cb.command = kControlPrmEnd;
    ByteWriter w;
    writeControlBlock(w, cb);
    NdrArgs res;
    if (!call(rpc::kOpControl, w.data(), res, timeoutMs) || !res.status.ok()) {
        log("PrmEnd failed " + res.status.str());
        return false;
    }
    std::unique_lock<std::mutex> lock(m_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return appReadyReceived_; })) {
        log("no ApplicationReady from the device");
        return false;
    }
    log("ApplicationReady received and confirmed");
    return true;
}

std::vector<uint8_t> ControllerSim::buildOutputFrame()
{
    ByteWriter w;
    writeEthHeader(w, device_.mac, cfg_.port->info().mac, kEtherTypeProfinet, cfg_.vlan, kVlanTciRt);
    w.u16be(outputFrameId_);
    const size_t csdu = w.size();
    w.zeros(outputCsdu_);
    std::vector<uint8_t>& f = w.data();
    {
        std::lock_guard<std::mutex> lock(m_);
        std::memcpy(&f[csdu], outPattern_.data(), cfg_.outputSize);
    }
    const size_t out = cfg_.outputSize;
    f[csdu + out] = kIoxsGood; // IOPS slot 2
    for (size_t i = 1; i <= 4; ++i)
        f[csdu + out + i] = kIoxsGood; // IOCS for the device's submodules
    cycleCounter_ = uint16_t(cycleCounter_ + kSendClockFactor1ms * cfg_.reductionRatio);
    w.u16be(cycleCounter_);
    w.u8(kDataStatusDefault);
    w.u8(0);
    std::vector<uint8_t> frame = std::move(f);
    padFrame(frame);
    return frame;
}

void ControllerSim::cyclicLoop()
{
    // Same scheduling as the device: absolute due times, one-shot timer, no drift.
    softeip::DeadlineTimer timer;
    const auto period = std::chrono::milliseconds(cfg_.reductionRatio);
    auto next = Clock::now();
    while (running_) {
        timer.waitUntil(next);
        const auto now = Clock::now();
        next += period;
        if (next <= now)
            next = now + period;
        if (!cyclicOn_)
            continue;
        cfg_.port->send(buildOutputFrame());
        ++framesSent_;
    }
}

void ControllerSim::exchange(int ms)
{
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    uint8_t value = 1;
    while (Clock::now() < end) {
        {
            std::lock_guard<std::mutex> lock(m_);
            prevPattern_ = outPattern_;
            for (size_t i = 4; i < outPattern_.size(); ++i)
                outPattern_[i] = uint8_t(value + i);
        }
        ++value;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool ControllerSim::release(int timeoutMs)
{
    ControlBlock cb;
    cb.type = block::kReleaseReq;
    cb.arUuid = arUuid_;
    cb.sessionKey = sessionKey_;
    cb.command = kControlRelease;
    ByteWriter w;
    writeControlBlock(w, cb);
    NdrArgs res;
    const bool ok = call(rpc::kOpRelease, w.data(), res, timeoutMs) && res.status.ok();
    cyclicOn_ = false;
    return ok;
}

} // namespace softpn
```

### Key Concepts
1. **Echo check** tolerates one cycle of lag after each data change.
2. **RTA ERR detection**: proves the device notifies the controller when it aborts.

---

## Step 9.14.3: `pn_controller_sim.cpp`

**File:** `tools/pn_controller_sim.cpp`

### Purpose
Command-line front end with a step list and RESULT: PASS/FAIL.

### Implementation

```cpp
// Minimal PROFINET IO-Controller (PLC side) to test a softpn device over a real NIC.
// It runs the same sequence as a PLC: DCP Identify, Connect, Write, Read I&M0, PrmEnd,
// ApplicationReady, cyclic exchange with an echo check (against pn_device_demo / fb_device_demo),
// Release.
//
// Usage: pn_controller_sim --adapter <name|mac|ip> [--name softpn-device] [--set-name-mac MAC]
//                          [--in-size 32] [--out-size 32] [--rr 8] [--seconds 5] [--local-port 34965]
//                          [--device-port 34964] [--wrong-module 1] [--advanced 1] [--skip-release 1] [--vlan 0]
//                          [--dhf 3]   data-hold factor: frames the device may miss before it aborts
//
// Run it on a second PC (or a second NIC cabled to the device NIC). On the same NIC as the device,
// Npcap's loopback of sent frames may also work [verify]; RPC then goes over the local IP stack.
#include "pn_controller.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace softpn;

int main(int argc, char** argv)
{
    ControllerConfig cc;
    std::string adapter;
    std::string setNameMac;
    int seconds = 5;
    bool skipRelease = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        unsigned long n = std::strtoul(v, nullptr, 0);
        if (k == "--adapter") adapter = v;
        else if (k == "--name") cc.deviceName = v;
        else if (k == "--set-name-mac") setNameMac = v;
        else if (k == "--in-size") cc.inputSize = n;
        else if (k == "--out-size") cc.outputSize = n;
        else if (k == "--rr") cc.reductionRatio = uint16_t(n);
        else if (k == "--seconds") seconds = int(n);
        else if (k == "--local-port") cc.localRpcPort = uint16_t(n);
        else if (k == "--device-port") cc.deviceRpcPort = uint16_t(n);
        else if (k == "--wrong-module") cc.wrongModule = n != 0;
        else if (k == "--advanced") cc.advancedStartup = n != 0;
        else if (k == "--vlan") cc.vlan = n != 0;
        else if (k == "--dhf") cc.dataHoldFactor = uint16_t(n);
        else if (k == "--skip-release") skipRelease = n != 0;
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }
    if (adapter.empty()) {
        std::fprintf(stderr, "--adapter is required (pn_probe --list shows the adapters)\n");
        return 2;
    }
    std::string err;
    L2Options opt;
    std::unique_ptr<L2Port> port = openL2Port(adapter, opt, &err);
    if (!port) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    cc.port = std::move(port);
    cc.log = [](const std::string& m) { std::printf("%s\n", m.c_str()); };
    ControllerSim plc(cc);
    if (!plc.start(&err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }

    int failures = 0;
    auto step = [&](bool ok, const char* what) {
        std::printf("%-28s %s\n", what, ok ? "OK" : "FAILED");
        if (!ok)
            ++failures;
        return ok;
    };

    if (!setNameMac.empty()) {
        MacAddress mac;
        step(MacAddress::parse(setNameMac, mac) && plc.setName(mac, cc.deviceName), "DCP Set NameOfStation");
    }
    if (step(plc.identify(), "DCP Identify")) {
        const PnioStatus st = plc.connect();
        if (step(st.ok(), "RPC Connect")) {
            if (!plc.moduleDiff().empty())
                std::printf("  ModuleDiffBlock with %zu module(s)\n", plc.moduleDiff().size());
            step(plc.writeRecords(), "RPC Write (PDev record)");
            Im0Data im0;
            if (step(plc.readIm0(im0), "RPC Read I&M0"))
                std::printf("  I&M0: vendor 0x%04X, order '%s', serial '%s'\n", im0.vendorId, im0.orderId.c_str(),
                            im0.serialNumber.c_str());
            if (step(plc.prmEnd(), "PrmEnd + ApplicationReady")) {
                plc.exchange(seconds * 1000);
                std::printf("  cyclic: sent %llu, received %llu, echo matches %llu, device IOPS %s\n",
                            (unsigned long long)plc.framesSent(), (unsigned long long)plc.framesReceived(),
                            (unsigned long long)plc.echoMatches(), plc.inputIopsGood() ? "GOOD" : "BAD");
                const uint64_t expected = uint64_t(seconds) * 1000 / cc.reductionRatio;
                step(plc.framesReceived() * 10 >= expected * 9, "cyclic frames >= 90 %");
                step(cc.wrongModule || plc.echoMatches() * 2 >= plc.framesReceived(), "echo (with pn_device_demo)");
            }
            if (!skipRelease)
                step(plc.release(), "RPC Release");
            else
                std::printf("skipping Release: the device watchdog must end the AR\n");
        } else {
            std::printf("  PNIOStatus %s\n", st.str().c_str());
        }
    }
    plc.stop();
    std::printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
```

### Key Concepts
1. **`--dhf N`**: tolerance of the device watchdog; the PLC default is 3.
2. **`--rr 4`**: must be refused (`DB 81 02 0B`) with the default minimum of 8 ms.

---

## Step 9.14.4: Build the simulator

**File:** `CMakeLists.txt`

### Purpose
Inside `if(SOFTPN_ENABLED)`:

### Implementation

```cmake
# The controller simulator class is shared by the CLI tool and the offline self-test.
add_library(pn_controller STATIC tools/pn_controller.cpp tools/pn_controller.hpp)
target_include_directories(pn_controller PUBLIC src/pn tools)
target_link_libraries(pn_controller PUBLIC softpn)
softeip_warnings(pn_controller)

add_executable(pn_controller_sim tools/pn_controller_sim.cpp)
target_link_libraries(pn_controller_sim PRIVATE pn_controller)
softeip_warnings(pn_controller_sim)
set_target_properties(pn_controller pn_controller_sim PROPERTIES FOLDER "tools")
```

### Key Concepts
1. **Static helper library** instead of compiling `pn_controller.cpp` twice.

---

## Step 9.14.5: Run it

### Purpose
Real Ethernet between device and simulator.

### Procedure
Two PCs (or two NICs cabled together): device on one, simulator on the other. Both work on Windows
(Npcap) and Linux.

- **Never the same NIC**: the device drops every frame that carries its own MAC (that is how it
  filters out its own transmissions), and the simulator would send with that same MAC.
- **Two NICs in one PC** (e.g. a USB-Ethernet dongle) cabled to each other: give both NICs IPs in
  the same subnet. RPC then goes through Windows' own IP stack, the PROFINET frames over the cable.

```bat
pn_device_demo    --adapter "Ethernet 2" --name softpn-device
pn_controller_sim --adapter "Ethernet 3" --name softpn-device --seconds 60
pn_controller_sim --adapter "Ethernet 3" --name softpn-device --rr 4            :: expect refusal
pn_controller_sim --adapter "Ethernet 3" --name softpn-device --skip-release 1  :: device watchdog ends the AR
pn_controller_sim --adapter "Ethernet 3" --name softpn-device --wrong-module 1  :: ModuleDiff, IOPS BAD
```
Linux without hardware, as root, with two network namespaces joined by a veth pair:
```sh
ip netns add pndev; ip netns add pnplc
ip link add vdev type veth peer name vplc
ip link set vdev netns pndev; ip link set vplc netns pnplc
ip -n pndev addr add 192.168.77.10/24 dev vdev; ip -n pndev link set vdev up
ip -n pnplc addr add 192.168.77.20/24 dev vplc; ip -n pnplc link set vplc up
ip netns exec pndev ./pn_device_demo --adapter vdev --name veth-device &
ip netns exec pnplc ./pn_controller_sim --adapter vplc --name veth-device --seconds 3
ip netns del pndev; ip netns del pnplc
```
A device on Windows with the simulator inside WSL (on the WSL virtual switch) checks the protocol, but
not the timing: that path has 36-72 ms gaps, so use `--dhf 10` there.

---

## Measured results

| Setup | Result |
|---|---|
| Linux veth + netns (raw AF_PACKET) | PASS: identify, connect, write, I&M0, PrmEnd/AppReady, 373/373 echoes in 3 s, release; `--rr 4` refused `DB 81 02 0B`; skip-release -> watchdog abort |
| Windows device (Npcap on WSL vNIC), simulator in WSL | DCP, Connect, Write, Read, PrmEnd/AppReady, ModuleDiff, IOPS BAD OK; cyclic 1251/1251 with `--dhf 10` for 10 s. With `--dhf 3` the AR ends within ~1 s because the WSL path has gaps up to 36 ms (sender) / 72 ms (receiver) - a test-bed limit, not the device (device TX max 8.5-8.7 ms) |
| Simulator on Windows (Npcap) | to do: so far it ran only on Linux / inside WSL; it builds on Windows with 0 warnings |
| Two machines / CODESYS | to do |

---

## Verification Checklist

- [ ] Linux veth: all steps OK, cyclic >= 90 %, echo OK, RESULT: PASS
- [ ] `--rr 4` refused with `DB 81 02 0B`; `--skip-release 1` -> device log "data-hold timer expired"
- [ ] Two machines on real Ethernet: 60 s at 8 ms with the default `--dhf 3`, RESULT: PASS
- [ ] Simulator run on Windows (second PC or second NIC): RESULT: PASS

---

## Next Phase
Continue to [Phase9.15-SelfTest.md](Phase9.15-SelfTest.md): Offline self-test (pn_selftest).
