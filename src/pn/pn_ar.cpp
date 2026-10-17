#include "pn_ar.hpp"

#include "pn_defs.hpp"

#include <algorithm>
#include <cstdio>

namespace softpn {

const char* arStateName(ArState s)
{
    switch (s) {
    case ArState::Idle: return "Idle";
    case ArState::WaitPrmEnd: return "WaitPrmEnd";
    case ArState::WaitAppReadyCnf: return "WaitAppReadyCnf";
    case ArState::Data: return "Data";
    }
    return "?";
}

namespace {

constexpr uint16_t kOurAlarmReference = 0x0001;
constexpr uint16_t kMaxAlarmDataLength = 200;

PnioStatus connectError(uint8_t code1, uint8_t code2)
{
    return PnioStatus::make(err::kCodeConnect, err::kDecodePnio, code1, code2);
}

bool isPowerOfTwo(unsigned v) { return v != 0 && (v & (v - 1)) == 0; }

// Data length the controller expects for (api, slot, subslot) in one direction (1 = input, 2 = output).
uint16_t expectedLength(const std::vector<ExpectedModule>& expected, uint32_t api, uint16_t slot, uint16_t subslot,
                        uint16_t direction)
{
    for (const ExpectedModule& m : expected) {
        if (m.api != api || m.slot != slot)
            continue;
        for (const ExpectedSubmodule& s : m.submodules)
            if (s.subslot == subslot)
                for (const DataDescription& d : s.data)
                    if (d.type == direction)
                        return d.length;
    }
    return 0;
}

std::vector<uint8_t> blocksOf(ByteWriter& w) { return std::move(w.data()); }

} // namespace

ArManager::ArManager(ArConfig config, const DeviceModel& model, Hooks hooks)
    : cfg_(std::move(config)), model_(model), hooks_(std::move(hooks))
{
}

std::string ArManager::lastAbort() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastAbort_;
}

void ArManager::setState(ArState s)
{
    if (state_.exchange(s) != s) {
        log(std::string("AR: ") + arStateName(s));
        if (hooks_.stateChanged)
            hooks_.stateChanged(s);
    }
}

bool ArManager::handleRequest(const RpcRequest& req, std::vector<uint8_t>& responseBody)
{
    if (req.header.interfaceId != pnioDeviceInterface()) {
        log("RPC: request for interface " + req.header.interfaceId.str() + " opnum " +
            std::to_string(req.header.opnum) + " from " + req.from.str() + " rejected (not the PNIO device interface)");
        return false; // e.g. the endpoint mapper: not supported
    }
    NdrArgs args;
    try {
        args = parseNdrRequest(req.body.data(), req.body.size(), req.header.littleEndian);
    } catch (const ParseError&) {
        responseBody = buildNdrResponse(
            PnioStatus::make(err::kCodeConnect, err::kDecodePnio, err::kCode1Cmrpc, err::kCmrpcArgsLengthInvalid), 0, {});
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    switch (req.header.opnum) {
    case rpc::kOpConnect: responseBody = onConnect(req, args); return true;
    case rpc::kOpRelease: responseBody = onRelease(args); return true;
    case rpc::kOpRead: responseBody = onRead(args, false); return true;
    case rpc::kOpWrite: responseBody = onWrite(args); return true;
    case rpc::kOpControl: responseBody = onControl(args); return true;
    case rpc::kOpReadImplicit: responseBody = onRead(args, true); return true;
    default:
        log("RPC: unsupported opnum " + std::to_string(req.header.opnum) + " from " + req.from.str() + " rejected");
        return false;
    }
}

PnioStatus ArManager::validateConnect(const ConnectRequest& c) const
{
    using namespace err;
    if (!c.hasAr)
        return connectError(kCode1Cmrpc, kCmrpcUnknownBlocks);
    if (c.ar.arType != kArTypeIocarSingle)
        return connectError(kCode1ArBlock, 4); // field 4 = ARType [verify field numbering]
    const IocrBlockReq* in = nullptr;
    const IocrBlockReq* out = nullptr;
    for (const IocrBlockReq& cr : c.iocrs) {
        if (cr.iocrType == kIocrTypeInput && !in)
            in = &cr;
        else if (cr.iocrType == kIocrTypeOutput && !out)
            out = &cr;
        else
            return connectError(kCode1IocrBlock, 4); // IOCRType
    }
    if (!in || !out)
        return connectError(kCode1Cmrpc, kCmrpcIocrMissing);
    for (const IocrBlockReq* cr : {in, out}) {
        if (cr->lt != kEtherTypeProfinet)
            return connectError(kCode1IocrBlock, 6);
        const uint32_t rtClass = cr->iocrProperties & kIocrPropRtClassMask;
        if (rtClass != 1 && rtClass != 2) // RT_CLASS_1 (legacy value 1, current value 2) [verify]
            return connectError(kCode1IocrBlock, 7);
        if (cr->dataLength < kMinCsduLength || cr->dataLength > kMaxCsduLength)
            return connectError(kCode1IocrBlock, 8);
        const bool deviceChooses = cr->iocrType == kIocrTypeOutput && cr->frameId == kFrameIdDeviceChooses;
        if (!deviceChooses && (cr->frameId < kFrameIdRtClass1First || cr->frameId > kFrameIdRtClass1Last))
            return connectError(kCode1IocrBlock, 9);
        if (cr->sendClockFactor != kSendClockFactor1ms)
            return connectError(kCode1IocrBlock, 10);
        if (!isPowerOfTwo(cr->reductionRatio) || cr->reductionRatio > 512)
            return connectError(kCode1IocrBlock, 11);
        const uint64_t updateUs = uint64_t(cr->sendClockFactor) * cr->reductionRatio * kTimeBaseNs / 1000;
        if (updateUs < uint64_t(cfg_.minUpdateTimeMs) * 1000)
            return connectError(kCode1IocrBlock, 11);
        if (cr->watchdogFactor == 0)
            return connectError(kCode1IocrBlock, 15);
        if (cr->dataHoldFactor == 0)
            return connectError(kCode1IocrBlock, 16);
        // Every data object, its IOPS and every IOCS must fit into the C_SDU.
        const uint16_t dir = cr->iocrType == kIocrTypeInput ? 1 : 2;
        for (const IocrApi& api : cr->apis) {
            for (const IoDataObject& o : api.ioData) {
                const uint16_t len = expectedLength(c.expected, api.api, o.slot, o.subslot, dir);
                if (size_t(o.frameOffset) + len + 1 > cr->dataLength)
                    return connectError(kCode1IocrBlock, 20); // IODataObjectFrameOffset [verify]
            }
            for (const IoDataObject& o : api.iocs)
                if (size_t(o.frameOffset) + 1 > cr->dataLength)
                    return connectError(kCode1IocrBlock, 23); // IOCSFrameOffset [verify]
        }
    }
    if (c.alarmCrs.size() != 1)
        return connectError(kCode1Cmrpc, kCmrpcWrongAlarmCrCount);
    if (c.alarmCrs[0].lt != kEtherTypeProfinet)
        return connectError(kCode1AlarmCrBlock, 5);
    if (c.expected.empty())
        return connectError(kCode1ExpectedSubmodule, 4);
    return PnioStatus{};
}

std::vector<uint8_t> ArManager::onConnect(const RpcRequest& req, const NdrArgs& args)
{
    ConnectRequest c;
    PnioStatus st = parseConnectRequest(args.blocks.data(), args.blocks.size(), c);
    if (st.ok())
        st = validateConnect(c);
    if (st.ok() && state_ != ArState::Idle) {
        if (c.ar.arUuid == arUuid_) {
            abortLocked(err::kRtaAbortRerun, false, "controller re-connected with the same ARUUID");
        } else {
            st = connectError(err::kCode1Cmrpc, err::kCmrpcOutOfArResources);
            log("AR: Connect refused, an AR is already active");
        }
    }
    if (!st.ok()) {
        std::string crs;
        for (const IocrBlockReq& cr : c.iocrs) {
            char buf[96];
            std::snprintf(buf, sizeof buf, " [IOCR type %u FrameID 0x%04X props 0x%08X len %u]", unsigned(cr.iocrType),
                          unsigned(cr.frameId), unsigned(cr.iocrProperties), unsigned(cr.dataLength));
            crs += buf;
        }
        log("AR: Connect refused, PNIOStatus " + st.str() + crs);
        return buildNdrResponse(st, args.argsMaximum, {});
    }

    const IocrBlockReq* in = nullptr;
    const IocrBlockReq* out = nullptr;
    for (const IocrBlockReq& cr : c.iocrs)
        (cr.iocrType == kIocrTypeInput ? in : out) = &cr;
    if (!in || !out) // guaranteed by validateConnect(); repeated for the code analyzer
        return buildNdrResponse(connectError(err::kCode1Cmrpc, err::kCmrpcIocrMissing), args.argsMaximum, {});
    const AlarmCrBlockReq& alarm = c.alarmCrs[0];
    const ConfigCheck check = compareExpected(model_, c.expected);

    // ---- cyclic layout from the IOCR blocks -----------------------------------------------------
    CyclicSetup cs;
    cs.ownMac = cfg_.mac;
    cs.controllerMac = c.ar.cmInitiatorMac;
    cs.vlan = cfg_.vlan;
    cs.inputFrameId = in->frameId;
    cs.inputDataLength = in->dataLength;
    cs.cycleCounterIncrement = uint16_t(in->sendClockFactor * in->reductionRatio);
    cs.periodUs = uint32_t(uint64_t(in->sendClockFactor) * in->reductionRatio * kTimeBaseNs / 1000);
    for (const IocrApi& api : in->apis) {
        for (const IoDataObject& o : api.ioData) {
            const bool proper = check.proper.count(submoduleKey(api.api, o.slot, o.subslot)) != 0;
            const uint16_t len = expectedLength(c.expected, api.api, o.slot, o.subslot, 1);
            if (proper && o.slot == kSlotInput) {
                cs.inputDataOffset = o.frameOffset;
                cs.inputDataLen = len;
            }
            cs.inputIops.push_back({uint16_t(o.frameOffset + len), proper});
        }
        for (const IoDataObject& o : api.iocs)
            cs.inputIocs.push_back({o.frameOffset, check.proper.count(submoduleKey(api.api, o.slot, o.subslot)) != 0});
    }
    // Output CR FrameID 0xFFFF: the device (the receiver) chooses it and returns it in the IOCRBlockRes.
    uint16_t outputFrameId = out->frameId;
    if (outputFrameId == kFrameIdDeviceChooses)
        outputFrameId = in->frameId != kOurOutputFrameId ? kOurOutputFrameId : uint16_t(kOurOutputFrameId + 1);
    cs.outputFrameId = outputFrameId;
    cs.outputDataLength = out->dataLength;
    const uint64_t outPeriodUs = uint64_t(out->sendClockFactor) * out->reductionRatio * kTimeBaseNs / 1000;
    cs.dataHoldUs = uint32_t(outPeriodUs * out->dataHoldFactor);
    for (const IocrApi& api : out->apis) {
        for (const IoDataObject& o : api.ioData) {
            if (o.slot != kSlotOutput || !check.proper.count(submoduleKey(api.api, o.slot, o.subslot)))
                continue;
            const uint16_t len = expectedLength(c.expected, api.api, o.slot, o.subslot, 2);
            cs.outputDataOffset = o.frameOffset;
            cs.outputDataLen = len;
            cs.outputIopsOffset = o.frameOffset + len;
        }
    }

    // ---- response blocks --------------------------------------------------------------------------
    ByteWriter w;
    // CMResponderUDPRTPort is the RT_CLASS_UDP port 0x8892 (like CMInitiatorUDPRTPort), not the RPC port.
    writeArBlockRes(w, ArBlockRes{c.ar.arType, c.ar.arUuid, c.ar.sessionKey, cfg_.mac, kUdpRtPort});
    for (const IocrBlockReq& cr : c.iocrs)
        writeIocrBlockRes(w, IocrBlockRes{cr.iocrType, cr.iocrReference,
                                          &cr == out ? outputFrameId : cr.frameId});
    writeAlarmCrBlockRes(w, AlarmCrBlockRes{alarm.alarmCrType, kOurAlarmReference,
                                            std::min(alarm.maxAlarmDataLength, kMaxAlarmDataLength)});
    if (!check.diff.empty()) {
        writeModuleDiffBlock(w, check.diff);
        log("AR: expected configuration differs (" + std::to_string(check.diff.size()) +
            " module(s) in ModuleDiffBlock): check the module sizes in the controller project");
    }
    if (c.ar.arProperties & kArPropStartupAdvanced)
        writeArServerBlock(w, hooks_.stationName ? hooks_.stationName() : std::string());

    // ---- activate ---------------------------------------------------------------------------------
    arUuid_ = c.ar.arUuid;
    sessionKey_ = c.ar.sessionKey;
    initiatorObject_ = c.ar.cmInitiatorObjectUuid;
    // ApplicationReady goes to the controller's PNIO-CM RPC port (34964). CMInitiatorUDPRTPort (0x8892) in the
    // ARBlockReq is the RT_CLASS_UDP port and not an RPC endpoint (the CODESYS PN-Controller does not answer there).
    controllerRpc_ = UdpAddress{req.from.ip, cfg_.controllerRpcPort};
    const unsigned timeoutMs = 100u * std::max<unsigned>(c.ar.activityTimeoutFactor, 1u);
    deadline_ = Clock::now() + std::chrono::milliseconds(timeoutMs);
    appReadyActivity_ = Uuid{};
    appReadyTries_ = 0;
    ++connects_;

    RtaSetup rs;
    rs.ownMac = cfg_.mac;
    rs.controllerMac = c.ar.cmInitiatorMac;
    rs.localRef = kOurAlarmReference;
    rs.remoteRef = alarm.localAlarmReference;
    rs.tagHigh = alarm.tagHeaderHigh;
    rs.tagLow = alarm.tagHeaderLow;
    rs.vlan = cfg_.vlan;
    if (hooks_.startRta)
        hooks_.startRta(rs);
    if (hooks_.startCyclic)
        hooks_.startCyclic(cs);
    log("AR: connected to '" + c.ar.cmInitiatorStationName + "' (" + c.ar.cmInitiatorMac.str() + "), update time " +
        std::to_string(cs.periodUs / 1000) + " ms, C_SDU in/out " + std::to_string(cs.inputDataLength) + "/" +
        std::to_string(cs.outputDataLength) + " bytes, Connect from " + req.from.str() + ", output FrameID " +
        std::to_string(cs.outputFrameId));
    setState(ArState::WaitPrmEnd);
    return buildNdrResponse(PnioStatus{}, args.argsMaximum, blocksOf(w));
}

std::vector<uint8_t> ArManager::onRelease(const NdrArgs& args)
{
    using namespace err;
    ControlBlock cb;
    try {
        size_t used = 0;
        cb = parseControlBlock(readBlock(args.blocks.data(), args.blocks.size(), used));
    } catch (const ParseError&) {
        return buildNdrResponse(PnioStatus::make(kCodeRelease, kDecodePnio, kCode1ReleaseBlock, 1), args.argsMaximum, {});
    }
    if (state_ == ArState::Idle || cb.arUuid != arUuid_)
        return buildNdrResponse(PnioStatus::make(kCodeRelease, kDecodePnio, kCode1Cmrpc, kCmrpcArUuidUnknown),
                                args.argsMaximum, {});
    ByteWriter w;
    ControlBlock res = cb;
    res.type = block::kReleaseRes;
    res.command = kControlDone;
    writeControlBlock(w, res);
    abortLocked(kRtaAbortRelease, false, "released by the controller");
    return buildNdrResponse(PnioStatus{}, args.argsMaximum, blocksOf(w));
}

std::vector<uint8_t> ArManager::onControl(const NdrArgs& args)
{
    using namespace err;
    ControlBlock cb;
    try {
        size_t used = 0;
        cb = parseControlBlock(readBlock(args.blocks.data(), args.blocks.size(), used));
    } catch (const ParseError&) {
        return buildNdrResponse(PnioStatus::make(kCodeControl, kDecodePnio, kCode1ControlBlock, 1), args.argsMaximum, {});
    }
    if (state_ == ArState::Idle || cb.arUuid != arUuid_)
        return buildNdrResponse(PnioStatus::make(kCodeControl, kDecodePnio, kCode1Cmrpc, kCmrpcArUuidUnknown),
                                args.argsMaximum, {});
    if (cb.type != block::kPrmEndReq || !(cb.command & kControlPrmEnd) || state_ != ArState::WaitPrmEnd) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "AR: Control block 0x%04X command 0x%04X refused in state %s",
                      unsigned(cb.type), unsigned(cb.command), arStateName(state_));
        log(buf);
        return buildNdrResponse(PnioStatus::make(kCodeControl, kDecodePnio, kCode1Cmrpc, kCmrpcStateConflict),
                                args.argsMaximum, {});
    }
    ByteWriter w;
    ControlBlock res = cb;
    res.type = block::kPrmEndRes;
    res.command = kControlDone;
    writeControlBlock(w, res);
    if (hooks_.setApplicationReady)
        hooks_.setApplicationReady(true); // parameterisation done: our IOPS become GOOD
    appReadyActivity_ = Uuid::random();
    appReadyNext_ = Clock::now(); // sent by poll() right after this response went out
    appReadyTries_ = 0;
    setState(ArState::WaitAppReadyCnf);
    return buildNdrResponse(PnioStatus{}, args.argsMaximum, blocksOf(w));
}

void ArManager::logRecord(const char* what, const RecordHeader& h, size_t len, const PnioStatus& st)
{
    char buf[128];
    std::snprintf(buf, sizeof buf, "AR: %s slot %u subslot 0x%04X index 0x%04X, %u bytes -> %s", what,
                  unsigned(h.slot), unsigned(h.subslot), unsigned(h.index), unsigned(len),
                  st.ok() ? "OK" : ("PNIOStatus " + st.str()).c_str());
    log(buf);
}

PnioStatus ArManager::writeRecord(const RecordHeader& h, const uint8_t* data, size_t len)
{
    using namespace err;
    (void)data;
    PnioStatus st;
    if (!model_.find(h.api, h.slot, h.subslot))
        st = PnioStatus::make(kCodeWrite, kDecodePnioRw, kRwInvalidSlotSubslot, 0);
    // Slot 0 (DAP, interface, port): PDev records such as PDPortDataCheck (0x802B) or
    // PDPortDataAdjust (0x802F). Accepted and ignored: topology and port settings are not
    // checked by this software device. IO modules have no parameters.
    else if (h.slot != 0)
        st = PnioStatus::make(kCodeWrite, kDecodePnioRw, kRwInvalidIndex, 0);
    logRecord("Write", h, len, st);
    return st;
}

std::vector<uint8_t> ArManager::onWrite(const NdrArgs& args)
{
    using namespace err;
    const uint8_t* p = args.blocks.data();
    const size_t n = args.blocks.size();
    RecordHeader h;
    size_t used = 0;
    try {
        h = parseRecordHeader(readBlock(p, n, used));
        if (h.type != block::kWriteReqHeader || used + h.recordDataLength > n)
            throw ParseError("write data truncated");
    } catch (const ParseError&) {
        return buildNdrResponse(PnioStatus::make(kCodeWrite, kDecodePnioRw, kRwWriteLengthError, 0), args.argsMaximum, {});
    }
    if (state_ == ArState::Idle || h.arUuid != arUuid_)
        return buildNdrResponse(PnioStatus::make(kCodeWrite, kDecodePnio, kCode1Cmrpc, kCmrpcArUuidUnknown),
                                args.argsMaximum, {});

    ByteWriter w;
    PnioStatus overall;
    RecordHeader res = h;
    res.type = block::kWriteResHeader;
    if (h.index != kIndexMultipleWrite) {
        res.status = writeRecord(h, p + used, h.recordDataLength);
        overall = res.status;
        writeRecordHeader(w, res);
    } else {
        // Multiple write: the data is a list of write headers + data, each padded to 4 bytes.
        ByteWriter sub;
        const uint8_t* d = p + used;
        size_t off = 0;
        try {
            while (off < h.recordDataLength) {
                size_t u = 0;
                RecordHeader sh = parseRecordHeader(readBlock(d + off, h.recordDataLength - off, u));
                if (off + u + sh.recordDataLength > h.recordDataLength)
                    throw ParseError("multiple write entry truncated");
                RecordHeader sr = sh;
                sr.type = block::kWriteResHeader;
                sr.status = writeRecord(sh, d + off + u, sh.recordDataLength);
                if (!sr.status.ok() && overall.ok())
                    overall = sr.status;
                writeRecordHeader(sub, sr);
                off += u + sh.recordDataLength;
                while (off % 4 != 0 && off < h.recordDataLength)
                    ++off;
            }
        } catch (const ParseError&) {
            return buildNdrResponse(PnioStatus::make(kCodeWrite, kDecodePnioRw, kRwWriteLengthError, 0),
                                    args.argsMaximum, {});
        }
        res.recordDataLength = uint32_t(sub.size());
        res.status = overall;
        writeRecordHeader(w, res);
        w.bytes(sub.data());
    }
    return buildNdrResponse(overall, args.argsMaximum, blocksOf(w));
}

std::vector<uint8_t> ArManager::onRead(const NdrArgs& args, bool implicit)
{
    using namespace err;
    RecordHeader h;
    try {
        size_t used = 0;
        h = parseRecordHeader(readBlock(args.blocks.data(), args.blocks.size(), used));
        if (h.type != block::kReadReqHeader)
            throw ParseError("not a read request");
    } catch (const ParseError&) {
        return buildNdrResponse(PnioStatus::make(kCodeRead, kDecodePnioRw, kRwInvalidIndex, 0), args.argsMaximum, {});
    }
    if (!implicit && (state_ == ArState::Idle || h.arUuid != arUuid_))
        return buildNdrResponse(PnioStatus::make(kCodeRead, kDecodePnio, kCode1Cmrpc, kCmrpcArUuidUnknown),
                                args.argsMaximum, {});

    PnioStatus st;
    ByteWriter data;
    if (!model_.find(h.api, h.slot, h.subslot)) {
        st = PnioStatus::make(kCodeRead, kDecodePnioRw, kRwInvalidSlotSubslot, 0);
    } else if (h.index == kIndexIm0) {
        Im0Data im0 = cfg_.im0;
        im0.vendorId = cfg_.vendorId;
        writeIm0Block(data, im0);
    } else {
        st = PnioStatus::make(kCodeRead, kDecodePnioRw, kRwInvalidIndex, 0); // [verify] reads CODESYS issues
    }
    if (data.size() > h.recordDataLength && st.ok())
        st = PnioStatus::make(kCodeRead, kDecodePnioRw, kRwWriteLengthError, 0);
    logRecord(implicit ? "Read (implicit)" : "Read", h, st.ok() ? data.size() : 0, st);

    ByteWriter w;
    RecordHeader res = h;
    res.type = block::kReadResHeader;
    res.recordDataLength = st.ok() ? uint32_t(data.size()) : 0;
    writeRecordHeader(w, res);
    if (st.ok())
        w.bytes(data.data());
    return buildNdrResponse(st, args.argsMaximum, blocksOf(w));
}

void ArManager::sendApplicationReady()
{
    ControlBlock cb;
    cb.type = block::kApplicationReadyReq;
    cb.arUuid = arUuid_;
    cb.sessionKey = sessionKey_;
    cb.command = kControlApplicationReady;
    ByteWriter w;
    writeControlBlock(w, cb);
    const std::vector<uint8_t> body = buildNdrRequest(256, w.data());
    if (hooks_.sendRpcRequest)
        hooks_.sendRpcRequest(controllerRpc_, initiatorObject_, pnioControllerInterface(), rpc::kOpControl,
                              appReadyActivity_, 0, body);
    ++appReadyTries_;
    log("AR: ApplicationReady sent to " + controllerRpc_.str() + (appReadyTries_ > 1 ? " (retry)" : ""));
}

void ArManager::handleResponse(const RpcHeader& header, const std::vector<uint8_t>& body, const UdpAddress& from)
{
    (void)from;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ArState::WaitAppReadyCnf || header.activity != appReadyActivity_)
        return;
    if (header.ptype != rpc::kPtypeResponse) {
        abortLocked(err::kRtaAbortRequested, true, "controller rejected ApplicationReady (RPC fault/reject)");
        return;
    }
    try {
        NdrArgs res = parseNdrResponse(body.data(), body.size(), header.littleEndian);
        if (!res.status.ok()) {
            abortLocked(err::kRtaAbortRequested, true, "controller answered ApplicationReady with " + res.status.str());
            return;
        }
        size_t used = 0;
        ControlBlock cb = parseControlBlock(readBlock(res.blocks.data(), res.blocks.size(), used));
        if (cb.type != block::kApplicationReadyRes || !(cb.command & kControlDone) || cb.arUuid != arUuid_)
            return;
    } catch (const ParseError&) {
        return;
    }
    if (hooks_.armWatchdog)
        hooks_.armWatchdog();
    setState(ArState::Data);
}

void ArManager::poll(Clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const ArState s = state_;
    if (s == ArState::Idle || s == ArState::Data)
        return;
    if (now > deadline_) {
        abortLocked(err::kRtaAbortCmiTimeout, true,
                    s == ArState::WaitPrmEnd ? "no PrmEnd within the activity timeout"
                                             : "no ApplicationReady confirmation within the activity timeout");
        return;
    }
    if (s == ArState::WaitAppReadyCnf && now >= appReadyNext_) {
        sendApplicationReady();
        appReadyNext_ = now + std::chrono::seconds(1);
    }
}

void ArManager::abort(uint8_t reasonCode2, bool notifyController, const std::string& why)
{
    std::lock_guard<std::mutex> lock(mutex_);
    abortLocked(reasonCode2, notifyController, why);
}

void ArManager::abortLocked(uint8_t reasonCode2, bool notifyController, const std::string& why)
{
    if (state_ == ArState::Idle)
        return;
    if (notifyController && hooks_.sendRtaError)
        hooks_.sendRtaError(PnioStatus::make(err::kCodeRta, err::kDecodePnio, err::kCode1RtaProtocol, reasonCode2));
    if (hooks_.stopCyclic)
        hooks_.stopCyclic();
    if (hooks_.stopRta)
        hooks_.stopRta();
    lastAbort_ = why;
    if (reasonCode2 != err::kRtaAbortRelease)
        ++aborts_; // a normal release is not an abort
    log("AR: ended: " + why);
    setState(ArState::Idle);
}

} // namespace softpn
