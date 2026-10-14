#include "pn_dcp.hpp"

#include "pn_defs.hpp"

#include <algorithm>
#include <cctype>

namespace softpn {

// ---- codec ---------------------------------------------------------------------------------------

DcpPdu parseDcp(const uint8_t* p, size_t n)
{
    ByteReader r(p, n);
    DcpPdu pdu;
    pdu.frameId = r.u16be();
    pdu.serviceId = r.u8();
    pdu.serviceType = r.u8();
    pdu.xid = r.u32be();
    pdu.responseDelay = r.u16be();
    const uint16_t dataLength = r.u16be();
    if (dataLength > r.remaining())
        throw ParseError("DCPDataLength beyond frame");
    ByteReader br(r.cur(), dataLength);
    const bool bareGet = pdu.serviceId == dcp::kServiceGet && pdu.serviceType == dcp::kTypeRequest;
    while (br.remaining() >= 2) {
        DcpBlock b;
        b.option = br.u8();
        b.suboption = br.u8();
        if (bareGet) {
            b.bare = true;
            pdu.blocks.push_back(b);
            continue;
        }
        const uint16_t len = br.u16be();
        const uint8_t* d = br.take(len);
        b.data.assign(d, d + len);
        if ((len & 1) && br.remaining() > 0)
            br.skip(1); // padding to even length
        pdu.blocks.push_back(std::move(b));
    }
    return pdu;
}

std::vector<uint8_t> serializeDcp(const DcpPdu& pdu)
{
    ByteWriter w;
    w.u16be(pdu.frameId);
    w.u8(pdu.serviceId);
    w.u8(pdu.serviceType);
    w.u32be(pdu.xid);
    w.u16be(pdu.responseDelay);
    const size_t lenPos = w.size();
    w.u16be(0);
    for (const DcpBlock& b : pdu.blocks) {
        w.u8(b.option);
        w.u8(b.suboption);
        if (b.bare)
            continue;
        w.u16be(uint16_t(b.data.size()));
        w.bytes(b.data);
        if (b.data.size() & 1)
            w.u8(0);
    }
    w.patchU16be(lenPos, uint16_t(w.size() - lenPos - 2));
    return std::move(w.data());
}

std::vector<uint8_t> buildDcpFrame(const MacAddress& dst, const MacAddress& src, const DcpPdu& pdu)
{
    ByteWriter w;
    writeEthHeader(w, dst, src, kEtherTypeProfinet);
    w.bytes(serializeDcp(pdu));
    std::vector<uint8_t> f = std::move(w.data());
    padFrame(f);
    return f;
}

DcpBlock makeDcpBlock(uint8_t option, uint8_t suboption, uint16_t infoOrQualifier, const void* payload, size_t n)
{
    DcpBlock b;
    b.option = option;
    b.suboption = suboption;
    b.data.push_back(uint8_t(infoOrQualifier >> 8));
    b.data.push_back(uint8_t(infoOrQualifier));
    auto p = static_cast<const uint8_t*>(payload);
    if (p && n)
        b.data.insert(b.data.end(), p, p + n);
    return b;
}

std::vector<uint8_t> dcpPayload(const DcpBlock& b)
{
    if (b.data.size() <= 2)
        return {};
    return std::vector<uint8_t>(b.data.begin() + 2, b.data.end());
}

uint16_t dcpQualifier(const DcpBlock& b)
{
    return b.data.size() >= 2 ? uint16_t((b.data[0] << 8) | b.data[1]) : 0;
}

bool isValidStationName(const std::string& name, std::string* why)
{
    auto fail = [why](const char* msg) {
        if (why)
            *why = msg;
        return false;
    };
    if (name.empty())
        return true;
    if (name.size() > 240)
        return fail("longer than 240 characters");
    size_t labelStart = 0;
    bool allDigitsOrDots = true;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '.') {
            const size_t len = i - labelStart;
            if (len == 0)
                return fail("empty label");
            if (len > 63)
                return fail("label longer than 63 characters");
            if (name[labelStart] == '-' || name[i - 1] == '-')
                return fail("label starts or ends with '-'");
            labelStart = i + 1;
            continue;
        }
        const char c = name[i];
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-'))
            return fail("only a-z, 0-9, '-' and '.' are allowed");
        if (!std::isdigit(static_cast<unsigned char>(c)))
            allDigitsOrDots = false;
    }
    if (allDigitsOrDots)
        return fail("must not look like an IP address");
    // "port-xyz" and "port-xyz-abcde" (x, y, z, a..e digits) are reserved for LLDP port IDs.
    auto isPort = [&](size_t len) {
        if (name.size() < len || name.compare(0, 5, "port-") != 0)
            return false;
        for (size_t i = 5; i < 8; ++i)
            if (!std::isdigit(static_cast<unsigned char>(name[i])))
                return false;
        if (len == 8)
            return name.size() == 8 || name[8] == '.';
        if (name[8] != '-')
            return false;
        for (size_t i = 9; i < 14; ++i)
            if (!std::isdigit(static_cast<unsigned char>(name[i])))
                return false;
        return name.size() == 14 || name[14] == '.';
    };
    if (isPort(8) || isPort(14))
        return fail("'port-xyz' names are reserved");
    return true;
}

// ---- responder ---------------------------------------------------------------------------------

namespace {

void putIpSuite(std::vector<uint8_t>& v, uint32_t ip, uint32_t mask, uint32_t gw)
{
    for (uint32_t x : {ip, mask, gw})
        for (int s = 24; s >= 0; s -= 8)
            v.push_back(uint8_t(x >> s));
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

DcpBlock responseBlock(uint8_t option, uint8_t suboption, uint8_t error)
{
    DcpBlock b;
    b.option = dcp::kOptControl;
    b.suboption = dcp::kSubCtrlResponse;
    b.data = {option, suboption, error};
    return b;
}

} // namespace

DcpResponder::DcpResponder(Config config, Hooks hooks) : cfg_(std::move(config)), hooks_(std::move(hooks)) {}

void DcpResponder::setStation(const StationData& data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    station_ = data;
}

StationData DcpResponder::station() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return station_;
}

void DcpResponder::setIpSuite(uint32_t ip, uint32_t mask, uint32_t gateway)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ip_ = ip;
    mask_ = mask;
    gateway_ = gateway;
}

// Caller holds mutex_.
bool DcpResponder::getBlock(uint8_t option, uint8_t suboption, DcpBlock& out) const
{
    using namespace dcp;
    if (option == kOptIp && suboption == kSubIpMac) {
        out = makeDcpBlock(option, suboption, 0, cfg_.mac.b.data(), 6);
    } else if (option == kOptIp && suboption == kSubIpParameter) {
        std::vector<uint8_t> v;
        putIpSuite(v, ip_, mask_, gateway_);
        out = makeDcpBlock(option, suboption, station_.ipSet ? kBlockInfoIpSet : 0, v.data(), v.size());
    } else if (option == kOptDeviceProperties && suboption == kSubDevVendor) {
        out = makeDcpBlock(option, suboption, 0, cfg_.vendorValue);
    } else if (option == kOptDeviceProperties && suboption == kSubDevNameOfStation) {
        out = makeDcpBlock(option, suboption, 0, station_.name);
    } else if (option == kOptDeviceProperties && suboption == kSubDevId) {
        const uint8_t id[4] = {uint8_t(cfg_.vendorId >> 8), uint8_t(cfg_.vendorId), uint8_t(cfg_.deviceId >> 8),
                               uint8_t(cfg_.deviceId)};
        out = makeDcpBlock(option, suboption, 0, id, 4);
    } else if (option == kOptDeviceProperties && suboption == kSubDevRole) {
        const uint8_t role[2] = {kDeviceRoleIoDevice, 0};
        out = makeDcpBlock(option, suboption, 0, role, 2);
    } else if (option == kOptDeviceProperties && suboption == kSubDevOptions) {
        static const uint8_t supported[] = {
            kOptIp, kSubIpMac, kOptIp, kSubIpParameter,
            kOptDeviceProperties, kSubDevVendor, kOptDeviceProperties, kSubDevNameOfStation,
            kOptDeviceProperties, kSubDevId, kOptDeviceProperties, kSubDevRole,
            kOptDeviceProperties, kSubDevOptions, kOptDeviceProperties, kSubDevInstance,
            kOptControl, kSubCtrlStart, kOptControl, kSubCtrlStop,
            kOptControl, kSubCtrlSignal, kOptControl, kSubCtrlResponse,
            kOptControl, kSubCtrlFactoryReset, kOptControl, kSubCtrlResetToFactory};
        out = makeDcpBlock(option, suboption, 0, supported, sizeof supported);
    } else if (option == kOptDeviceProperties && suboption == kSubDevInstance) {
        const uint8_t inst[2] = {0x00, 0x01};
        out = makeDcpBlock(option, suboption, 0, inst, 2);
    } else {
        return false;
    }
    return true;
}

// Caller holds mutex_.
std::vector<DcpBlock> DcpResponder::identityBlocks() const
{
    using namespace dcp;
    const uint8_t order[][2] = {{kOptDeviceProperties, kSubDevVendor},    {kOptDeviceProperties, kSubDevNameOfStation},
                                {kOptDeviceProperties, kSubDevId},        {kOptDeviceProperties, kSubDevRole},
                                {kOptDeviceProperties, kSubDevOptions},   {kOptDeviceProperties, kSubDevInstance},
                                {kOptIp, kSubIpParameter}};
    std::vector<DcpBlock> blocks;
    for (const auto& o : order) {
        DcpBlock b;
        if (getBlock(o[0], o[1], b))
            blocks.push_back(std::move(b));
    }
    return blocks;
}

// Caller holds mutex_.
bool DcpResponder::identifyMatches(const DcpPdu& req) const
{
    using namespace dcp;
    if (req.blocks.empty())
        return false;
    for (const DcpBlock& b : req.blocks) {
        if (b.option == kOptAllSelector && b.suboption == kSubAllSelector)
            continue;
        const std::vector<uint8_t>& d = b.data; // filter blocks carry no BlockInfo
        if (b.option == kOptDeviceProperties && b.suboption == kSubDevNameOfStation) {
            if (station_.name.empty() || toLower(std::string(d.begin(), d.end())) != toLower(station_.name))
                return false;
        } else if (b.option == kOptDeviceProperties && b.suboption == kSubDevId) {
            if (d.size() != 4 || uint16_t((d[0] << 8) | d[1]) != cfg_.vendorId ||
                uint16_t((d[2] << 8) | d[3]) != cfg_.deviceId)
                return false;
        } else if (b.option == kOptDeviceProperties && b.suboption == kSubDevVendor) {
            if (std::string(d.begin(), d.end()) != cfg_.vendorValue)
                return false;
        } else if (b.option == kOptDeviceProperties && b.suboption == kSubDevAliasName) {
            return false; // alias names need LLDP neighbourhood data: not supported
        } else if (b.option == kOptIp && b.suboption == kSubIpParameter) {
            if (d.size() < 4 || ((uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3]) != ip_)
                return false;
        }
        // Other filter options (DeviceRole, DeviceInstance...) do not exclude us.
    }
    return true;
}

// Caller holds mutex_. Returns the BlockError for one Set block.
uint8_t DcpResponder::handleSet(const DcpBlock& b, bool& changedPersistent, bool& changedName)
{
    using namespace dcp;
    const uint16_t qualifier = dcpQualifier(b);
    const bool permanent = (qualifier & 0x0001) != 0;
    const std::vector<uint8_t> payload = dcpPayload(b);
    const bool busy = hooks_.inOperation && hooks_.inOperation();

    if (b.option == kOptControl) {
        switch (b.suboption) {
        case kSubCtrlStart:
        case kSubCtrlStop:
            return kErrOk; // transaction brackets sent by some controllers
        case kSubCtrlSignal:
            log("DCP: signal (flash LED) requested");
            if (hooks_.onSignal)
                hooks_.onSignal();
            return kErrOk;
        case kSubCtrlFactoryReset:
        case kSubCtrlResetToFactory:
            if (busy)
                return kErrInOperation;
            log("DCP: reset to factory settings (NameOfStation cleared)");
            station_ = StationData{};
            changedPersistent = changedName = true;
            return kErrOk;
        default:
            return kErrSuboptionUnsupported;
        }
    }
    if (b.option == kOptDeviceProperties && b.suboption == kSubDevNameOfStation) {
        std::string name(payload.begin(), payload.end());
        std::string why;
        if (!isValidStationName(name, &why)) {
            log("DCP: refused NameOfStation '" + name + "': " + why);
            return kErrSuboptionNotSet;
        }
        if (busy && name != station_.name)
            return kErrInOperation;
        log("DCP: NameOfStation = '" + name + "'" + (permanent ? " (permanent)" : " (temporary)"));
        changedName = name != station_.name;
        station_.name = name;
        changedPersistent = changedPersistent || permanent;
        return kErrOk;
    }
    if (b.option == kOptIp && b.suboption == kSubIpParameter) {
        if (payload.size() < 12)
            return kErrSuboptionNotSet;
        auto rd = [&](size_t o) {
            return (uint32_t(payload[o]) << 24) | (uint32_t(payload[o + 1]) << 16) | (uint32_t(payload[o + 2]) << 8) |
                   payload[o + 3];
        };
        const uint32_t ip = rd(0), mask = rd(4), gw = rd(8);
        if (ip == ip_ && mask == mask_) {
            (void)gw; // the gateway is not checked: Windows owns the routing table
            station_.ipSet = true;
            changedPersistent = changedPersistent || permanent;
            return kErrOk;
        }
        if (busy)
            return kErrInOperation;
        log("DCP: refused IP " + ipToString(ip) + "/" + ipToString(mask) + ": configure the controller with the PC's "
            "static IP " + ipToString(ip_) + "/" + ipToString(mask_));
        return kErrLocalReasons;
    }
    if (b.option == kOptIp || b.option == kOptDeviceProperties)
        return kErrSuboptionUnsupported;
    return kErrOptionUnsupported;
}

void DcpResponder::onFrame(const EthHeader& eth, const uint8_t* frame, size_t len)
{
    using namespace dcp;
    DcpPdu req;
    try {
        req = parseDcp(frame + eth.payloadOffset, len - eth.payloadOffset);
    } catch (const ParseError&) {
        return; // malformed: ignore silently, as the spec requires
    }
    if (req.serviceType != kTypeRequest)
        return; // responses from other devices

    const bool toUs = eth.dst == cfg_.mac;
    const MacAddress identifyGroup = MacAddress::fromBytes(kMulticastIdentify);

    if (req.frameId == kFrameIdDcpIdentifyReq && req.serviceId == kServiceIdentify &&
        (eth.dst == identifyGroup || toUs)) {
        std::vector<uint8_t> out;
        unsigned delayMs = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!identifyMatches(req))
                return;
            DcpPdu res;
            res.frameId = kFrameIdDcpIdentifyRes;
            res.serviceId = kServiceIdentify;
            res.serviceType = kTypeResponseSuccess;
            res.xid = req.xid;
            res.blocks = identityBlocks();
            out = buildDcpFrame(eth.src, cfg_.mac, res);
            if (req.responseDelay > 1) {
                // Spread answers to an Identify-All over ResponseDelay x 10 ms (derived from our MAC).
                const unsigned slot = ((unsigned(cfg_.mac.b[4]) << 8) | cfg_.mac.b[5]) % req.responseDelay;
                delayMs = slot * kResponseDelayUnitMs;
            }
            if (delayMs > 0) {
                pending_.push_back({Clock::now() + std::chrono::milliseconds(delayMs), std::move(out)});
                return;
            }
        }
        hooks_.send(out);
        return;
    }

    if (req.frameId != kFrameIdDcpGetSet || !toUs)
        return;

    DcpPdu res;
    res.frameId = kFrameIdDcpGetSet;
    res.serviceId = req.serviceId;
    res.serviceType = kTypeResponseSuccess;
    res.xid = req.xid;
    bool persist = false, nameChanged = false;
    StationData snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (req.serviceId == kServiceGet) {
            for (const DcpBlock& b : req.blocks) {
                DcpBlock out;
                if (getBlock(b.option, b.suboption, out))
                    res.blocks.push_back(std::move(out));
                else
                    res.blocks.push_back(responseBlock(b.option, b.suboption, kErrSuboptionUnsupported));
            }
        } else if (req.serviceId == kServiceSet) {
            for (const DcpBlock& b : req.blocks)
                res.blocks.push_back(responseBlock(b.option, b.suboption, handleSet(b, persist, nameChanged)));
        } else {
            res.serviceType = kTypeResponseNotSupported;
        }
        snapshot = station_;
    }
    if (persist && hooks_.persist)
        hooks_.persist(snapshot); // before the answer: the controller may power-cycle us right after it
    hooks_.send(buildDcpFrame(eth.src, cfg_.mac, res));
    if (nameChanged && hooks_.onIdentityChanged)
        hooks_.onIdentityChanged();
}

void DcpResponder::poll(Clock::time_point now)
{
    std::vector<std::vector<uint8_t>> due;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->due <= now) {
                due.push_back(std::move(it->frame));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& f : due)
        hooks_.send(f);
}

} // namespace softpn
