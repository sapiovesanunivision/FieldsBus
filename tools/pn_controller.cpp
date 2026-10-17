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
    ar.cmInitiatorUdpPort = 0x8892; // RT_CLASS_UDP port as sent by real controllers, not our RPC port
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
