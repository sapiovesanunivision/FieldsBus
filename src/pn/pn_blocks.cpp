#include "pn_blocks.hpp"

#include "pn_defs.hpp"

#include <algorithm>

namespace softpn {

namespace {

ByteReader bodyReader(const RawBlock& b) { return ByteReader(b.body, b.bodyLength); }

void expectType(const RawBlock& b, uint16_t type)
{
    if (b.type != type)
        throw ParseError("unexpected block type");
}

MacAddress readMac(ByteReader& r) { return MacAddress::fromBytes(r.take(6)); }
void writeMac(ByteWriter& w, const MacAddress& m) { w.bytes(m.b.data(), 6); }

std::string fixedString(const std::string& s, size_t n)
{
    std::string out = s.substr(0, n);
    out.resize(n, ' ');
    return out;
}

} // namespace

RawBlock readBlock(const uint8_t* p, size_t n, size_t& consumed)
{
    if (n < 6)
        throw ParseError("block header truncated");
    RawBlock b;
    b.type = uint16_t((p[0] << 8) | p[1]);
    const uint16_t length = uint16_t((p[2] << 8) | p[3]);
    if (length < 2 || size_t(length) + 4 > n)
        throw ParseError("block length beyond buffer");
    b.versionHigh = p[4];
    b.versionLow = p[5];
    b.body = p + 6;
    b.bodyLength = size_t(length) - 2;
    consumed = size_t(length) + 4;
    return b;
}

std::vector<RawBlock> splitBlocks(const uint8_t* p, size_t n)
{
    std::vector<RawBlock> blocks;
    size_t off = 0;
    while (off < n) {
        size_t used = 0;
        blocks.push_back(readBlock(p + off, n - off, used));
        off += used;
    }
    return blocks;
}

BlockWriter::BlockWriter(ByteWriter& w, uint16_t type, uint8_t versionHigh, uint8_t versionLow)
    : w_(w), start_(w.size())
{
    w_.u16be(type);
    w_.u16be(0); // patched by finish()
    w_.u8(versionHigh);
    w_.u8(versionLow);
}

void BlockWriter::finish()
{
    if (done_)
        return;
    done_ = true;
    w_.patchU16be(start_ + 2, uint16_t(w_.size() - start_ - 4));
}

// ---- Connect request -----------------------------------------------------------------------------

ArBlockReq parseArBlockReq(const RawBlock& b)
{
    expectType(b, block::kArBlockReq);
    ByteReader r = bodyReader(b);
    ArBlockReq a;
    a.arType = r.u16be();
    a.arUuid = readUuid(r, false);
    a.sessionKey = r.u16be();
    a.cmInitiatorMac = readMac(r);
    a.cmInitiatorObjectUuid = readUuid(r, false);
    a.arProperties = r.u32be();
    a.activityTimeoutFactor = r.u16be();
    a.cmInitiatorUdpPort = r.u16be();
    const uint16_t nameLength = r.u16be();
    const uint8_t* name = r.take(nameLength);
    a.cmInitiatorStationName.assign(name, name + nameLength);
    return a;
}

void writeArBlockReq(ByteWriter& w, const ArBlockReq& a)
{
    BlockWriter bw(w, block::kArBlockReq);
    w.u16be(a.arType);
    writeUuid(w, a.arUuid, false);
    w.u16be(a.sessionKey);
    writeMac(w, a.cmInitiatorMac);
    writeUuid(w, a.cmInitiatorObjectUuid, false);
    w.u32be(a.arProperties);
    w.u16be(a.activityTimeoutFactor);
    w.u16be(a.cmInitiatorUdpPort);
    w.u16be(uint16_t(a.cmInitiatorStationName.size()));
    w.bytes(a.cmInitiatorStationName.data(), a.cmInitiatorStationName.size());
}

IocrBlockReq parseIocrBlockReq(const RawBlock& b)
{
    expectType(b, block::kIocrBlockReq);
    ByteReader r = bodyReader(b);
    IocrBlockReq c;
    c.iocrType = r.u16be();
    c.iocrReference = r.u16be();
    c.lt = r.u16be();
    c.iocrProperties = r.u32be();
    c.dataLength = r.u16be();
    c.frameId = r.u16be();
    c.sendClockFactor = r.u16be();
    c.reductionRatio = r.u16be();
    c.phase = r.u16be();
    c.sequence = r.u16be();
    c.frameSendOffset = r.u32be();
    c.watchdogFactor = r.u16be();
    c.dataHoldFactor = r.u16be();
    c.tagHeader = r.u16be();
    c.multicastMac = readMac(r);
    const uint16_t apis = r.u16be();
    for (uint16_t i = 0; i < apis; ++i) {
        IocrApi api;
        api.api = r.u32be();
        const uint16_t nData = r.u16be();
        for (uint16_t k = 0; k < nData; ++k) {
            IoDataObject o;
            o.slot = r.u16be();
            o.subslot = r.u16be();
            o.frameOffset = r.u16be();
            api.ioData.push_back(o);
        }
        const uint16_t nIocs = r.u16be();
        for (uint16_t k = 0; k < nIocs; ++k) {
            IoDataObject o;
            o.slot = r.u16be();
            o.subslot = r.u16be();
            o.frameOffset = r.u16be();
            api.iocs.push_back(o);
        }
        c.apis.push_back(std::move(api));
    }
    return c;
}

void writeIocrBlockReq(ByteWriter& w, const IocrBlockReq& c)
{
    BlockWriter bw(w, block::kIocrBlockReq);
    w.u16be(c.iocrType);
    w.u16be(c.iocrReference);
    w.u16be(c.lt);
    w.u32be(c.iocrProperties);
    w.u16be(c.dataLength);
    w.u16be(c.frameId);
    w.u16be(c.sendClockFactor);
    w.u16be(c.reductionRatio);
    w.u16be(c.phase);
    w.u16be(c.sequence);
    w.u32be(c.frameSendOffset);
    w.u16be(c.watchdogFactor);
    w.u16be(c.dataHoldFactor);
    w.u16be(c.tagHeader);
    writeMac(w, c.multicastMac);
    w.u16be(uint16_t(c.apis.size()));
    for (const IocrApi& api : c.apis) {
        w.u32be(api.api);
        w.u16be(uint16_t(api.ioData.size()));
        for (const IoDataObject& o : api.ioData) {
            w.u16be(o.slot);
            w.u16be(o.subslot);
            w.u16be(o.frameOffset);
        }
        w.u16be(uint16_t(api.iocs.size()));
        for (const IoDataObject& o : api.iocs) {
            w.u16be(o.slot);
            w.u16be(o.subslot);
            w.u16be(o.frameOffset);
        }
    }
}

AlarmCrBlockReq parseAlarmCrBlockReq(const RawBlock& b)
{
    expectType(b, block::kAlarmCrBlockReq);
    ByteReader r = bodyReader(b);
    AlarmCrBlockReq a;
    a.alarmCrType = r.u16be();
    a.lt = r.u16be();
    a.properties = r.u32be();
    a.rtaTimeoutFactor = r.u16be();
    a.rtaRetries = r.u16be();
    a.localAlarmReference = r.u16be();
    a.maxAlarmDataLength = r.u16be();
    a.tagHeaderHigh = r.u16be();
    a.tagHeaderLow = r.u16be();
    return a;
}

void writeAlarmCrBlockReq(ByteWriter& w, const AlarmCrBlockReq& a)
{
    BlockWriter bw(w, block::kAlarmCrBlockReq);
    w.u16be(a.alarmCrType);
    w.u16be(a.lt);
    w.u32be(a.properties);
    w.u16be(a.rtaTimeoutFactor);
    w.u16be(a.rtaRetries);
    w.u16be(a.localAlarmReference);
    w.u16be(a.maxAlarmDataLength);
    w.u16be(a.tagHeaderHigh);
    w.u16be(a.tagHeaderLow);
}

std::vector<ExpectedModule> parseExpectedSubmoduleBlockReq(const RawBlock& b)
{
    expectType(b, block::kExpectedSubmoduleBlockReq);
    ByteReader r = bodyReader(b);
    std::vector<ExpectedModule> modules;
    const uint16_t apis = r.u16be();
    for (uint16_t i = 0; i < apis; ++i) {
        ExpectedModule m;
        m.api = r.u32be();
        m.slot = r.u16be();
        m.moduleIdent = r.u32be();
        m.moduleProperties = r.u16be();
        const uint16_t subs = r.u16be();
        for (uint16_t k = 0; k < subs; ++k) {
            ExpectedSubmodule s;
            s.subslot = r.u16be();
            s.ident = r.u32be();
            s.properties = r.u16be();
            const unsigned kind = s.properties & 0x3;
            const unsigned descriptions = kind == 3 ? 2 : 1; // "no IO" still has one (zero-length input)
            for (unsigned d = 0; d < descriptions; ++d) {
                DataDescription dd;
                dd.type = r.u16be();
                dd.length = r.u16be();
                dd.lengthIocs = r.u8();
                dd.lengthIops = r.u8();
                s.data.push_back(dd);
            }
            m.submodules.push_back(std::move(s));
        }
        modules.push_back(std::move(m));
    }
    return modules;
}

void writeExpectedSubmoduleBlockReq(ByteWriter& w, const ExpectedModule& m)
{
    BlockWriter bw(w, block::kExpectedSubmoduleBlockReq);
    w.u16be(1); // NumberOfAPIs
    w.u32be(m.api);
    w.u16be(m.slot);
    w.u32be(m.moduleIdent);
    w.u16be(m.moduleProperties);
    w.u16be(uint16_t(m.submodules.size()));
    for (const ExpectedSubmodule& s : m.submodules) {
        w.u16be(s.subslot);
        w.u32be(s.ident);
        w.u16be(s.properties);
        for (const DataDescription& d : s.data) {
            w.u16be(d.type);
            w.u16be(d.length);
            w.u8(d.lengthIocs);
            w.u8(d.lengthIops);
        }
    }
}

PnioStatus parseConnectRequest(const uint8_t* p, size_t n, ConnectRequest& out)
{
    using namespace err;
    std::vector<RawBlock> blocks;
    try {
        blocks = splitBlocks(p, n);
    } catch (const ParseError&) {
        return PnioStatus::make(kCodeConnect, kDecodePnio, kCode1Cmrpc, kCmrpcArgsLengthInvalid);
    }
    for (const RawBlock& b : blocks) {
        uint8_t code1 = 0;
        try {
            switch (b.type) {
            case block::kArBlockReq:
                code1 = kCode1ArBlock;
                out.ar = parseArBlockReq(b);
                out.hasAr = true;
                break;
            case block::kIocrBlockReq:
                code1 = kCode1IocrBlock;
                out.iocrs.push_back(parseIocrBlockReq(b));
                break;
            case block::kAlarmCrBlockReq:
                code1 = kCode1AlarmCrBlock;
                out.alarmCrs.push_back(parseAlarmCrBlockReq(b));
                break;
            case block::kExpectedSubmoduleBlockReq: {
                code1 = kCode1ExpectedSubmodule;
                auto mods = parseExpectedSubmoduleBlockReq(b);
                out.expected.insert(out.expected.end(), mods.begin(), mods.end());
                break;
            }
            default:
                ++out.unknownBlocks; // e.g. ARVendorBlockReq, PrmServerBlock: ignored
                break;
            }
        } catch (const ParseError&) {
            return PnioStatus::make(kCodeConnect, kDecodePnio, code1, 1); // field 1 = BlockLength
        }
    }
    return PnioStatus{};
}

// ---- Connect response ----------------------------------------------------------------------------

void writeArBlockRes(ByteWriter& w, const ArBlockRes& a)
{
    BlockWriter bw(w, block::kArBlockRes);
    w.u16be(a.arType);
    writeUuid(w, a.arUuid, false);
    w.u16be(a.sessionKey);
    writeMac(w, a.cmResponderMac);
    w.u16be(a.cmResponderUdpPort);
}

void writeIocrBlockRes(ByteWriter& w, const IocrBlockRes& c)
{
    BlockWriter bw(w, block::kIocrBlockRes);
    w.u16be(c.iocrType);
    w.u16be(c.iocrReference);
    w.u16be(c.frameId);
}

void writeAlarmCrBlockRes(ByteWriter& w, const AlarmCrBlockRes& a)
{
    BlockWriter bw(w, block::kAlarmCrBlockRes);
    w.u16be(a.alarmCrType);
    w.u16be(a.localAlarmReference);
    w.u16be(a.maxAlarmDataLength);
}

void writeModuleDiffBlock(ByteWriter& w, const std::vector<DiffModule>& diff)
{
    // Group modules by API (we only use API 0, but keep the format general).
    std::vector<uint32_t> apis;
    for (const DiffModule& m : diff)
        if (std::find(apis.begin(), apis.end(), m.api) == apis.end())
            apis.push_back(m.api);
    BlockWriter bw(w, block::kModuleDiffBlock);
    w.u16be(uint16_t(apis.size()));
    for (uint32_t api : apis) {
        w.u32be(api);
        uint16_t count = 0;
        for (const DiffModule& m : diff)
            if (m.api == api)
                ++count;
        w.u16be(count);
        for (const DiffModule& m : diff) {
            if (m.api != api)
                continue;
            w.u16be(m.slot);
            w.u32be(m.ident);
            w.u16be(m.state);
            w.u16be(uint16_t(m.submodules.size()));
            for (const DiffSubmodule& s : m.submodules) {
                w.u16be(s.subslot);
                w.u32be(s.ident);
                w.u16be(s.state);
            }
        }
    }
}

void writeArServerBlock(ByteWriter& w, const std::string& stationName)
{
    const size_t start = w.size();
    BlockWriter bw(w, block::kArServerBlock);
    w.u16be(uint16_t(stationName.size()));
    w.bytes(stationName.data(), stationName.size());
    while ((w.size() - start) % 4 != 0) // padding to a 32-bit boundary [verify]
        w.u8(0);
}

ArBlockRes parseArBlockRes(const RawBlock& b)
{
    expectType(b, block::kArBlockRes);
    ByteReader r = bodyReader(b);
    ArBlockRes a;
    a.arType = r.u16be();
    a.arUuid = readUuid(r, false);
    a.sessionKey = r.u16be();
    a.cmResponderMac = readMac(r);
    a.cmResponderUdpPort = r.u16be();
    return a;
}

IocrBlockRes parseIocrBlockRes(const RawBlock& b)
{
    expectType(b, block::kIocrBlockRes);
    ByteReader r = bodyReader(b);
    IocrBlockRes c;
    c.iocrType = r.u16be();
    c.iocrReference = r.u16be();
    c.frameId = r.u16be();
    return c;
}

AlarmCrBlockRes parseAlarmCrBlockRes(const RawBlock& b)
{
    expectType(b, block::kAlarmCrBlockRes);
    ByteReader r = bodyReader(b);
    AlarmCrBlockRes a;
    a.alarmCrType = r.u16be();
    a.localAlarmReference = r.u16be();
    a.maxAlarmDataLength = r.u16be();
    return a;
}

std::vector<DiffModule> parseModuleDiffBlock(const RawBlock& b)
{
    expectType(b, block::kModuleDiffBlock);
    ByteReader r = bodyReader(b);
    std::vector<DiffModule> diff;
    const uint16_t apis = r.u16be();
    for (uint16_t i = 0; i < apis; ++i) {
        const uint32_t api = r.u32be();
        const uint16_t modules = r.u16be();
        for (uint16_t k = 0; k < modules; ++k) {
            DiffModule m;
            m.api = api;
            m.slot = r.u16be();
            m.ident = r.u32be();
            m.state = r.u16be();
            const uint16_t subs = r.u16be();
            for (uint16_t s = 0; s < subs; ++s) {
                DiffSubmodule d;
                d.subslot = r.u16be();
                d.ident = r.u32be();
                d.state = r.u16be();
                m.submodules.push_back(d);
            }
            diff.push_back(std::move(m));
        }
    }
    return diff;
}

// ---- Control -------------------------------------------------------------------------------------

ControlBlock parseControlBlock(const RawBlock& b)
{
    switch (b.type) {
    case block::kPrmEndReq:
    case block::kPrmEndRes:
    case block::kApplicationReadyReq:
    case block::kApplicationReadyRes:
    case block::kReleaseReq:
    case block::kReleaseRes:
        break;
    default:
        throw ParseError("not a control block");
    }
    ByteReader r = bodyReader(b);
    ControlBlock c;
    c.type = b.type;
    r.skip(2); // reserved
    c.arUuid = readUuid(r, false);
    c.sessionKey = r.u16be();
    r.skip(2); // reserved
    c.command = r.u16be();
    c.properties = r.u16be();
    return c;
}

void writeControlBlock(ByteWriter& w, const ControlBlock& c)
{
    BlockWriter bw(w, c.type);
    w.u16be(0);
    writeUuid(w, c.arUuid, false);
    w.u16be(c.sessionKey);
    w.u16be(0);
    w.u16be(c.command);
    w.u16be(c.properties);
}

// ---- Record headers ------------------------------------------------------------------------------

RecordHeader parseRecordHeader(const RawBlock& b)
{
    if (b.type != block::kReadReqHeader && b.type != block::kWriteReqHeader && b.type != block::kReadResHeader &&
        b.type != block::kWriteResHeader)
        throw ParseError("not a record header");
    ByteReader r = bodyReader(b);
    RecordHeader h;
    h.type = b.type;
    h.seqNumber = r.u16be();
    h.arUuid = readUuid(r, false);
    h.api = r.u32be();
    h.slot = r.u16be();
    h.subslot = r.u16be();
    r.skip(2); // padding
    h.index = r.u16be();
    h.recordDataLength = r.u32be();
    if (b.type == block::kReadReqHeader) {
        if (r.remaining() >= 16)
            h.targetArUuid = readUuid(r, false); // zero for an explicit read
    } else if (b.type == block::kReadResHeader || b.type == block::kWriteResHeader) {
        h.additionalValue1 = r.u16be();
        h.additionalValue2 = r.u16be();
        if (b.type == block::kWriteResHeader) {
            h.status.code = r.u8();
            h.status.decode = r.u8();
            h.status.code1 = r.u8();
            h.status.code2 = r.u8();
        }
    }
    return h;
}

void writeRecordHeader(ByteWriter& w, const RecordHeader& h)
{
    const size_t start = w.size();
    BlockWriter bw(w, h.type);
    w.u16be(h.seqNumber);
    writeUuid(w, h.arUuid, false);
    w.u32be(h.api);
    w.u16be(h.slot);
    w.u16be(h.subslot);
    w.u16be(0); // padding
    w.u16be(h.index);
    w.u32be(h.recordDataLength);
    if (h.type == block::kReadReqHeader) {
        writeUuid(w, h.targetArUuid, false);
    } else if (h.type == block::kReadResHeader || h.type == block::kWriteResHeader) {
        w.u16be(h.additionalValue1);
        w.u16be(h.additionalValue2);
        if (h.type == block::kWriteResHeader) {
            w.u8(h.status.code);
            w.u8(h.status.decode);
            w.u8(h.status.code1);
            w.u8(h.status.code2);
        }
    }
    while (w.size() - start < block::kRecordHeaderLength + 4) // fixed size: BlockLength 60
        w.u8(0);
}

// ---- I&M0 ----------------------------------------------------------------------------------------

void writeIm0Block(ByteWriter& w, const Im0Data& m)
{
    BlockWriter bw(w, block::kIm0);
    w.u16be(m.vendorId);
    const std::string order = fixedString(m.orderId, 20);
    const std::string serial = fixedString(m.serialNumber, 16);
    w.bytes(order.data(), 20);
    w.bytes(serial.data(), 16);
    w.u16be(m.hardwareRevision);
    w.u8(uint8_t(m.swRevisionPrefix));
    w.bytes(m.swRevision, 3);
    w.u16be(m.revisionCounter);
    w.u16be(m.profileId);
    w.u16be(m.profileSpecificType);
    w.u16be(m.imVersion);
    w.u16be(m.imSupported);
}

Im0Data parseIm0Block(const RawBlock& b)
{
    expectType(b, block::kIm0);
    ByteReader r = bodyReader(b);
    Im0Data m;
    m.vendorId = r.u16be();
    const uint8_t* order = r.take(20);
    m.orderId.assign(order, order + 20);
    const uint8_t* serial = r.take(16);
    m.serialNumber.assign(serial, serial + 16);
    m.hardwareRevision = r.u16be();
    m.swRevisionPrefix = char(r.u8());
    const uint8_t* sw = r.take(3);
    m.swRevision[0] = sw[0];
    m.swRevision[1] = sw[1];
    m.swRevision[2] = sw[2];
    m.revisionCounter = r.u16be();
    m.profileId = r.u16be();
    m.profileSpecificType = r.u16be();
    m.imVersion = r.u16be();
    m.imSupported = r.u16be();
    return m;
}

} // namespace softpn
