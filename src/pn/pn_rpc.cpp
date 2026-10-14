#include "softeip/socket_compat.hpp" // first: winsock2.h before any windows.h

#include "pn_rpc.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <mutex>

namespace softpn {

using softeip::socket_t;

// ---- header / NDR codecs -------------------------------------------------------------------------

RpcHeader parseRpcHeader(const uint8_t* p, size_t n)
{
    if (n < rpc::kHeaderSize)
        throw ParseError("RPC header truncated");
    ByteReader r(p, n);
    if (r.u8() != 4)
        throw ParseError("not DCE/RPC version 4");
    RpcHeader h;
    h.ptype = r.u8();
    h.flags1 = r.u8();
    h.flags2 = r.u8();
    const uint8_t drep0 = r.u8();
    r.skip(2); // drep[1..2]: character and float representation
    h.littleEndian = (drep0 >> 4) == 1;
    const uint8_t serialHi = r.u8();
    (void)serialHi;
    h.object = readUuid(r, h.littleEndian);
    h.interfaceId = readUuid(r, h.littleEndian);
    h.activity = readUuid(r, h.littleEndian);
    auto u32 = [&] { return h.littleEndian ? r.u32() : r.u32be(); };
    auto u16 = [&] { return h.littleEndian ? r.u16() : r.u16be(); };
    h.serverBoot = u32();
    h.interfaceVersion = u32();
    h.seqnum = u32();
    h.opnum = u16();
    u16(); // interface hint
    u16(); // activity hint
    h.bodyLength = u16();
    h.fragnum = u16();
    r.skip(2); // auth_proto, serial_lo
    if (h.bodyLength > n - rpc::kHeaderSize)
        throw ParseError("RPC body length beyond datagram");
    return h;
}

void writeRpcHeader(ByteWriter& w, const RpcHeader& h, uint16_t bodyLength)
{
    w.u8(4);
    w.u8(h.ptype);
    w.u8(h.flags1);
    w.u8(h.flags2);
    w.u8(0x10); // drep: little-endian integers, ASCII, IEEE float
    w.u8(0x00);
    w.u8(0x00);
    w.u8(0x00); // serial_hi
    writeUuid(w, h.object, true);
    writeUuid(w, h.interfaceId, true);
    writeUuid(w, h.activity, true);
    w.u32(h.serverBoot);
    w.u32(h.interfaceVersion);
    w.u32(h.seqnum);
    w.u16(h.opnum);
    w.u16(0xFFFF); // interface hint: none
    w.u16(0xFFFF); // activity hint: none
    w.u16(bodyLength);
    w.u16(h.fragnum);
    w.u8(0); // auth_proto: none
    w.u8(0); // serial_lo
}

NdrArgs parseNdrRequest(const uint8_t* body, size_t n, bool le)
{
    ByteReader r(body, n);
    auto u32 = [&] { return le ? r.u32() : r.u32be(); };
    NdrArgs a;
    a.argsMaximum = u32();
    const uint32_t argsLength = u32();
    u32(); // MaximumCount
    u32(); // Offset
    const uint32_t actual = u32();
    if (actual > r.remaining() || argsLength > r.remaining())
        throw ParseError("NDR ActualCount beyond body");
    const uint8_t* p = r.take(actual);
    a.blocks.assign(p, p + actual);
    return a;
}

NdrArgs parseNdrResponse(const uint8_t* body, size_t n, bool le)
{
    ByteReader r(body, n);
    auto u32 = [&] { return le ? r.u32() : r.u32be(); };
    NdrArgs a;
    a.status.code = r.u8();
    a.status.decode = r.u8();
    a.status.code1 = r.u8();
    a.status.code2 = r.u8();
    if (r.remaining() < 16)
        return a; // error responses may stop after the PNIOStatus
    u32();        // ArgsLength
    a.argsMaximum = u32();
    u32(); // Offset
    const uint32_t actual = u32();
    if (actual > r.remaining())
        throw ParseError("NDR ActualCount beyond body");
    const uint8_t* p = r.take(actual);
    a.blocks.assign(p, p + actual);
    return a;
}

std::vector<uint8_t> buildNdrRequest(uint32_t argsMaximum, const std::vector<uint8_t>& blocks)
{
    ByteWriter w;
    w.u32(argsMaximum);
    w.u32(uint32_t(blocks.size())); // ArgsLength
    w.u32(argsMaximum);             // MaximumCount
    w.u32(0);                       // Offset
    w.u32(uint32_t(blocks.size())); // ActualCount
    w.bytes(blocks);
    return std::move(w.data());
}

std::vector<uint8_t> buildNdrResponse(const PnioStatus& status, uint32_t argsMaximum,
                                      const std::vector<uint8_t>& blocks)
{
    ByteWriter w;
    w.u8(status.code);
    w.u8(status.decode);
    w.u8(status.code1);
    w.u8(status.code2);
    w.u32(uint32_t(blocks.size())); // ArgsLength
    w.u32(argsMaximum);             // MaximumCount
    w.u32(0);                       // Offset
    w.u32(uint32_t(blocks.size())); // ActualCount
    w.bytes(blocks);
    return std::move(w.data());
}

// ---- endpoint ------------------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

sockaddr_in toSockaddr(const UdpAddress& a)
{
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(a.ip);
    s.sin_port = htons(a.port);
    return s;
}

UdpAddress fromSockaddr(const sockaddr_in& s) { return UdpAddress{ntohl(s.sin_addr.s_addr), ntohs(s.sin_port)}; }

struct CallKey {
    Uuid activity;
    uint32_t seqnum;
    bool operator<(const CallKey& o) const
    {
        if (activity != o.activity)
            return activity < o.activity;
        return seqnum < o.seqnum;
    }
};

struct Reassembly {
    RpcHeader first;
    std::map<uint16_t, std::vector<uint8_t>> fragments;
    int lastFragnum = -1;
    Clock::time_point started;
};

} // namespace

struct RpcEndpoint::Impl {
    socket_t sock = softeip::kInvalidSocket;
    uint16_t port = 0;
    uint32_t serverBoot = 0;
    RequestHandler onRequest;
    ResponseHandler onResponse;
    std::mutex sendMutex;
    std::map<CallKey, Reassembly> partial;
    // Last answered call per activity: retransmitted requests and pings get the same packets.
    std::map<Uuid, std::pair<uint32_t, std::vector<std::vector<uint8_t>>>> cache;
    std::deque<Uuid> cacheOrder;

    void sendRaw(const std::vector<uint8_t>& pkt, const UdpAddress& to)
    {
        std::lock_guard<std::mutex> lock(sendMutex);
        softeip::sendTo(sock, pkt.data(), pkt.size(), toSockaddr(to));
    }

    // Splits `body` into fragments of at most kMaxFragmentBody bytes.
    std::vector<std::vector<uint8_t>> packetize(RpcHeader h, const std::vector<uint8_t>& body)
    {
        std::vector<std::vector<uint8_t>> pkts;
        const size_t frags = body.empty() ? 1 : (body.size() + rpc::kMaxFragmentBody - 1) / rpc::kMaxFragmentBody;
        for (size_t i = 0; i < frags; ++i) {
            const size_t off = i * rpc::kMaxFragmentBody;
            const size_t len = std::min(rpc::kMaxFragmentBody, body.size() - off);
            h.fragnum = uint16_t(i);
            h.flags1 = uint8_t((h.flags1 & ~(rpc::kFlag1Frag | rpc::kFlag1LastFrag)) |
                               (frags > 1 ? rpc::kFlag1Frag : 0) | (i + 1 == frags ? rpc::kFlag1LastFrag : 0));
            ByteWriter w;
            writeRpcHeader(w, h, uint16_t(len));
            if (len)
                w.bytes(body.data() + off, len);
            pkts.push_back(std::move(w.data()));
        }
        return pkts;
    }

    void remember(const Uuid& activity, uint32_t seqnum, std::vector<std::vector<uint8_t>> pkts)
    {
        if (cache.find(activity) == cache.end()) {
            cacheOrder.push_back(activity);
            if (cacheOrder.size() > 16) {
                cache.erase(cacheOrder.front());
                cacheOrder.pop_front();
            }
        }
        cache[activity] = {seqnum, std::move(pkts)};
    }

    RpcHeader replyHeader(const RpcHeader& req, uint8_t ptype) const
    {
        RpcHeader h;
        h.ptype = ptype;
        h.flags1 = rpc::kFlag1NoFack;
        h.object = req.object;
        h.interfaceId = req.interfaceId;
        h.activity = req.activity;
        h.serverBoot = serverBoot;
        h.interfaceVersion = req.interfaceVersion;
        h.seqnum = req.seqnum;
        h.opnum = req.opnum;
        return h;
    }

    void sendSimple(const RpcHeader& req, uint8_t ptype, const std::vector<uint8_t>& body, const UdpAddress& to)
    {
        RpcHeader h = replyHeader(req, ptype);
        h.flags1 = uint8_t(h.flags1 | rpc::kFlag1LastFrag);
        ByteWriter w;
        writeRpcHeader(w, h, uint16_t(body.size()));
        w.bytes(body);
        sendRaw(w.data(), to);
    }

    void sendFack(const RpcHeader& req, const UdpAddress& to)
    {
        RpcHeader h = replyHeader(req, rpc::kPtypeFack);
        h.fragnum = req.fragnum;
        ByteWriter b;
        b.u8(0);          // fack version
        b.u8(0);          // pad
        b.u16(64);        // window size
        b.u32(0xFFFF);    // max TSDU
        b.u32(uint32_t(rpc::kHeaderSize + rpc::kMaxFragmentBody)); // max fragment size
        b.u16(0);         // serial number
        b.u16(0);         // selective ack length
        ByteWriter w;
        writeRpcHeader(w, h, uint16_t(b.size()));
        w.bytes(b.data());
        sendRaw(w.data(), to);
    }

    void handleRequestFragment(const RpcHeader& h, const uint8_t* body, const UdpAddress& from)
    {
        // Retransmission of a call we already answered: resend the cached answer.
        auto c = cache.find(h.activity);
        if (c != cache.end() && c->second.first == h.seqnum) {
            for (const auto& pkt : c->second.second)
                sendRaw(pkt, from);
            return;
        }

        std::vector<uint8_t> full;
        RpcHeader first = h;
        if (h.flags1 & rpc::kFlag1Frag) {
            if (!(h.flags1 & rpc::kFlag1NoFack) && !(h.flags1 & rpc::kFlag1LastFrag))
                sendFack(h, from);
            Reassembly& ra = partial[CallKey{h.activity, h.seqnum}];
            if (ra.fragments.empty()) {
                ra.started = Clock::now();
                ra.first = h;
            }
            ra.fragments[h.fragnum].assign(body, body + h.bodyLength);
            if (h.flags1 & rpc::kFlag1LastFrag)
                ra.lastFragnum = h.fragnum;
            if (ra.lastFragnum < 0 || ra.fragments.size() != size_t(ra.lastFragnum) + 1)
                return; // wait for the missing fragments
            for (auto& f : ra.fragments)
                full.insert(full.end(), f.second.begin(), f.second.end());
            first = ra.first;
            partial.erase(CallKey{h.activity, h.seqnum});
        } else {
            full.assign(body, body + h.bodyLength);
        }

        RpcRequest req;
        req.header = first;
        req.header.bodyLength = uint16_t(std::min<size_t>(full.size(), 0xFFFF));
        req.body = std::move(full);
        req.from = from;
        std::vector<uint8_t> responseBody;
        if (onRequest && onRequest(req, responseBody)) {
            auto pkts = packetize(replyHeader(first, rpc::kPtypeResponse), responseBody);
            for (const auto& pkt : pkts)
                sendRaw(pkt, from);
            remember(first.activity, first.seqnum, std::move(pkts));
        } else {
            ByteWriter st;
            st.u32(rpc::kNcaUnknownInterface);
            sendSimple(first, rpc::kPtypeReject, st.data(), from);
        }
    }

    void dropStalePartials()
    {
        const auto now = Clock::now();
        for (auto it = partial.begin(); it != partial.end();) {
            if (now - it->second.started > std::chrono::seconds(5))
                it = partial.erase(it);
            else
                ++it;
        }
    }
};

RpcEndpoint::RpcEndpoint() : impl_(std::make_unique<Impl>()) {}
RpcEndpoint::~RpcEndpoint() { close(); }

void RpcEndpoint::setRequestHandler(RequestHandler h) { impl_->onRequest = std::move(h); }
void RpcEndpoint::setResponseHandler(ResponseHandler h) { impl_->onResponse = std::move(h); }
void RpcEndpoint::setServerBoot(uint32_t bootTime) { impl_->serverBoot = bootTime; }
bool RpcEndpoint::isOpen() const { return impl_->sock != softeip::kInvalidSocket; }
uint16_t RpcEndpoint::port() const { return impl_->port; }

bool RpcEndpoint::open(uint16_t port, std::string* error, uint32_t bindIp)
{
    close();
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == softeip::kInvalidSocket) {
        if (error)
            *error = "RPC: cannot create UDP socket";
        return false;
    }
    sockaddr_in a = toSockaddr(UdpAddress{bindIp, port});
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        if (error)
            *error = "RPC: cannot bind UDP port " + std::to_string(port) + " (in use?)";
        softeip::closeSocket(s);
        return false;
    }
    softeip::disableUdpConnReset(s);
    impl_->sock = s;
    impl_->port = port;
    return true;
}

void RpcEndpoint::close()
{
    if (impl_->sock != softeip::kInvalidSocket) {
        softeip::closeSocket(impl_->sock);
        impl_->sock = softeip::kInvalidSocket;
    }
}

void RpcEndpoint::poll(int timeoutMs)
{
    Impl& d = *impl_;
    if (d.sock == softeip::kInvalidSocket)
        return;
    if (softeip::waitReadable(d.sock, unsigned(timeoutMs < 0 ? 0 : timeoutMs)) <= 0) {
        d.dropStalePartials();
        return;
    }
    uint8_t buf[2048];
    sockaddr_in fromSa{};
    int n = softeip::recvFrom(d.sock, buf, sizeof buf, fromSa);
    if (n <= 0)
        return;
    const UdpAddress from = fromSockaddr(fromSa);
    RpcHeader h;
    try {
        h = parseRpcHeader(buf, size_t(n));
    } catch (const ParseError&) {
        return;
    }
    const uint8_t* body = buf + rpc::kHeaderSize;
    switch (h.ptype) {
    case rpc::kPtypeRequest:
        d.handleRequestFragment(h, body, from);
        break;
    case rpc::kPtypePing: {
        auto c = d.cache.find(h.activity);
        if (c != d.cache.end() && c->second.first == h.seqnum) {
            for (const auto& pkt : c->second.second)
                d.sendRaw(pkt, from);
        } else if (d.partial.count(CallKey{h.activity, h.seqnum})) {
            d.sendSimple(h, rpc::kPtypeWorking, {}, from);
        } else {
            d.sendSimple(h, rpc::kPtypeNocall, {}, from);
        }
        break;
    }
    case rpc::kPtypeResponse:
    case rpc::kPtypeFault:
    case rpc::kPtypeReject:
        if (d.onResponse)
            d.onResponse(h, std::vector<uint8_t>(body, body + h.bodyLength), from);
        break;
    default:
        break; // ack, fack, working, nocall: nothing to do for our short calls
    }
}

bool RpcEndpoint::sendRequest(const UdpAddress& to, const Uuid& object, const Uuid& interfaceId, uint16_t opnum,
                              const Uuid& activity, uint32_t seqnum, const std::vector<uint8_t>& body)
{
    Impl& d = *impl_;
    if (d.sock == softeip::kInvalidSocket)
        return false;
    RpcHeader h;
    h.ptype = rpc::kPtypeRequest;
    h.flags1 = uint8_t(rpc::kFlag1NoFack | rpc::kFlag1Idempotent);
    h.object = object;
    h.interfaceId = interfaceId;
    h.activity = activity;
    h.serverBoot = 0; // client: unknown server boot time
    h.interfaceVersion = 1;
    h.seqnum = seqnum;
    h.opnum = opnum;
    for (const auto& pkt : d.packetize(h, body))
        d.sendRaw(pkt, to);
    return true;
}

} // namespace softpn
