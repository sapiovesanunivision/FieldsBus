// Software EtherNet/IP adapter: encapsulation, CIP objects, class-1 I/O.
#include "softeip/eip_adapter.hpp"

#include "softeip/bytes.hpp"
#include "softeip/cip_defs.hpp"
#include "softeip/socket_compat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

namespace softeip {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxTcpClients = 16;
constexpr size_t kMaxEncapPacket = encap::kHeaderSize + 0xFFFF;

std::string ipToString(const in_addr& addr)
{
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, const_cast<in_addr*>(&addr), buf, sizeof buf);
    return buf;
}

[[maybe_unused]] std::string hex16(uint16_t v)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%04X", v);
    return buf;
}

// Parsed EPATH. Only what an adapter needs: logical segments, electronic key,
// port segments (skipped) and a simple data segment.
struct ParsedPath {
    std::optional<uint16_t> classId;
    std::optional<uint16_t> instance;   // first instance segment
    std::optional<uint16_t> attribute;
    std::vector<uint16_t> instanceIds;  // all instance / connection-point segments, in order

    bool hasKey = false;
    uint16_t keyVendor = 0;
    uint16_t keyDeviceType = 0;
    uint16_t keyProductCode = 0;
    uint8_t keyMajor = 0;
    uint8_t keyMinor = 0;

    std::vector<uint8_t> data;
};

bool parsePath(const uint8_t* p, size_t n, ParsedPath& out)
{
    ByteReader r(p, n);
    try {
        while (r.remaining() > 0) {
            uint8_t seg = r.u8();
            if ((seg & 0xE0) == 0x00) {
                // Port segment: [seg][link addr size?][ext port?][link addr][pad]
                bool extendedLink = (seg & 0x10) != 0;
                size_t linkSize = extendedLink ? r.u8() : 1;
                if ((seg & 0x0F) == 0x0F)
                    r.u16();
                r.skip(linkSize);
                if (extendedLink && (linkSize & 1))
                    r.u8();
                continue;
            }
            switch (seg) {
            case 0x20: out.classId = r.u8(); break;
            case 0x21: r.u8(); out.classId = r.u16(); break;
            case 0x24:
            case 0x2C: {
                uint16_t v = r.u8();
                if (seg == 0x24 && !out.instance)
                    out.instance = v;
                out.instanceIds.push_back(v);
                break;
            }
            case 0x25:
            case 0x2D: {
                r.u8();
                uint16_t v = r.u16();
                if (seg == 0x25 && !out.instance)
                    out.instance = v;
                out.instanceIds.push_back(v);
                break;
            }
            case 0x30: out.attribute = r.u8(); break;
            case 0x31: r.u8(); out.attribute = r.u16(); break;
            case 0x34: {
                if (r.u8() != 0x04) // only key format 4 is defined
                    return false;
                out.hasKey = true;
                out.keyVendor = r.u16();
                out.keyDeviceType = r.u16();
                out.keyProductCode = r.u16();
                out.keyMajor = r.u8();
                out.keyMinor = r.u8();
                break;
            }
            case 0x80: {
                size_t words = r.u8();
                const uint8_t* d = r.take(words * 2);
                out.data.assign(d, d + words * 2);
                break;
            }
            default:
                return false;
            }
        }
    } catch (const ParseError&) {
        return false;
    }
    return true;
}

std::vector<uint8_t> cipReply(uint8_t service, uint8_t status, const std::vector<uint8_t>& data = {},
                              std::initializer_list<uint16_t> extStatus = {})
{
    ByteWriter w;
    w.u8(uint8_t(service | cip::kReplyFlag));
    w.u8(0);
    w.u8(status);
    w.u8(uint8_t(extStatus.size()));
    for (uint16_t e : extStatus)
        w.u16(e);
    w.bytes(data);
    return std::move(w.data());
}

struct TcpClient {
    socket_t sock = kInvalidSocket;
    sockaddr_in peer{};
    in_addr localIp{};
    uint32_t session = 0;
    std::vector<uint8_t> rx;
};

// Context of one explicit request (needed by Forward_Open).
struct RequestContext {
    sockaddr_in peer{};
    std::optional<uint16_t> t2oPort; // from Sockaddr Info T->O item
};

} // namespace

struct Adapter::Impl {
    explicit Impl(AdapterConfig c) : cfg(std::move(c)), inputData(cfg.inputSize, 0), outputData(cfg.outputSize, 0)
    {
        std::random_device rd;
        rng.seed(rd());
        nextSession = rng() | 1u;
        nextConnectionId = rng();
    }

    ~Impl() { stop(); }

    // ---- lifecycle --------------------------------------------------------

    bool start(std::string* error);
    void stop();
    void run();

    // ---- encapsulation ----------------------------------------------------

    void handleTcpReadable(TcpClient& c, bool& closeClient);
    std::optional<std::vector<uint8_t>> handleEncap(const uint8_t* pkt, size_t n, TcpClient* client,
                                                    const sockaddr_in& peer, in_addr localIp,
                                                    bool& closeClient);
    void writeIdentityItem(ByteWriter& w, in_addr localIp);
    void writeServicesItem(ByteWriter& w);
    std::vector<uint8_t> handleSendRRData(ByteReader& r, const sockaddr_in& peer, uint32_t& status);
    void handleUdpEncap();
    in_addr localIpFor(const sockaddr_in& peer) const;

    // ---- CIP --------------------------------------------------------------

    std::vector<uint8_t> handleCip(const uint8_t* msg, size_t n, RequestContext& ctx, int depth = 0);
    std::vector<uint8_t> handleIdentity(uint8_t service, const ParsedPath& path);
    std::vector<uint8_t> handleAssembly(uint8_t service, const ParsedPath& path);
    std::vector<uint8_t> handleConnectionManager(uint8_t service, ByteReader& body, RequestContext& ctx,
                                                 int depth);
    uint16_t identityStatus() const;

    // ---- helpers ----------------------------------------------------------

    void log(const std::string& msg) const
    {
        if (cfg.onLog)
            cfg.onLog(msg);
    }

    AdapterConfig cfg;
    SocketLibrary socketLib;

    std::thread thread;
    std::atomic<bool> running{false};

    socket_t tcpListen = kInvalidSocket;
    socket_t udpEncap = kInvalidSocket;
    socket_t udpIo = kInvalidSocket;
    in_addr bindIp{};
    std::vector<std::unique_ptr<TcpClient>> clients;

    std::mt19937 rng;
    uint32_t nextSession = 1;
    uint32_t nextConnectionId = 1;

    mutable std::mutex dataMutex;
    std::vector<uint8_t> inputData;  // guarded by dataMutex
    std::vector<uint8_t> outputData; // guarded by dataMutex
    std::atomic<bool> plcRun{false};
    std::atomic<bool> ownerConnected{false};
};

// ===========================================================================
// Lifecycle
// ===========================================================================

bool Adapter::Impl::start(std::string* error)
{
    auto fail = [&](const std::string& what) {
        if (error)
            *error = what;
        stop();
        return false;
    };

    if (inet_pton(AF_INET, cfg.bindAddress.c_str(), &bindIp) != 1)
        return fail("invalid bind address " + cfg.bindAddress);

    auto makeAddr = [&](uint16_t port) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr = bindIp;
        a.sin_port = htons(port);
        return a;
    };

    tcpListen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    udpEncap = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    udpIo = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tcpListen == kInvalidSocket || udpEncap == kInvalidSocket || udpIo == kInvalidSocket)
        return fail("socket() failed");

    for (socket_t s : {tcpListen, udpEncap, udpIo})
        setReuseAddr(s);
    disableUdpConnReset(udpEncap);
    disableUdpConnReset(udpIo);

    int on = 1;
    setsockopt(udpEncap, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);

    sockaddr_in a = makeAddr(kEncapPort);
    if (::bind(tcpListen, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
        return fail("cannot bind TCP 44818 (port in use?)");
    if (::listen(tcpListen, 8) != 0)
        return fail("listen() failed");
    if (::bind(udpEncap, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
        return fail("cannot bind UDP 44818 (port in use?)");
    a = makeAddr(kIoPort);
    if (::bind(udpIo, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
        return fail("cannot bind UDP 2222 (port in use?)");

    running = true;
    thread = std::thread([this] { run(); });
    log("adapter started on " + cfg.bindAddress + " (TCP/UDP 44818, UDP 2222)");
    return true;
}

void Adapter::Impl::stop()
{
    running = false;
    if (thread.joinable())
        thread.join();
    for (auto& c : clients)
        closeSocket(c->sock);
    clients.clear();
    for (socket_t* s : {&tcpListen, &udpEncap, &udpIo}) {
        if (*s != kInvalidSocket) {
            closeSocket(*s);
            *s = kInvalidSocket;
        }
    }
}

void Adapter::Impl::run()
{
#ifdef _WIN32
    if (cfg.raiseThreadPriority)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    while (running) {
        auto timeout = std::chrono::microseconds(50000);

        fd_set readSet;
        FD_ZERO(&readSet);
        socket_t maxFd = 0;
        auto add = [&](socket_t s) {
            FD_SET(s, &readSet);
            maxFd = std::max(maxFd, s);
        };
        add(tcpListen);
        add(udpEncap);
        add(udpIo);
        for (auto& c : clients)
            add(c->sock);

        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000000);
        tv.tv_usec = static_cast<long>(timeout.count() % 1000000);
        int ready = ::select(static_cast<int>(maxFd + 1), &readSet, nullptr, nullptr, &tv);
        if (ready < 0)
            continue;

        if (ready > 0) {
            if (FD_ISSET(tcpListen, &readSet)) {
                sockaddr_in peer{};
                socklen_t len = sizeof peer;
                socket_t s = ::accept(tcpListen, reinterpret_cast<sockaddr*>(&peer), &len);
                if (s != kInvalidSocket) {
                    if (clients.size() >= kMaxTcpClients) {
                        closeSocket(s);
                    } else {
                        auto c = std::make_unique<TcpClient>();
                        c->sock = s;
                        c->peer = peer;
                        sockaddr_in local{};
                        socklen_t llen = sizeof local;
                        getsockname(s, reinterpret_cast<sockaddr*>(&local), &llen);
                        c->localIp = local.sin_addr;
                        setNoDelay(s);
                        log("TCP session from " + ipToString(peer.sin_addr));
                        clients.push_back(std::move(c));
                    }
                }
            }
            if (FD_ISSET(udpEncap, &readSet))
                handleUdpEncap();
            for (size_t i = 0; i < clients.size();) {
                bool closeClient = false;
                if (FD_ISSET(clients[i]->sock, &readSet))
                    handleTcpReadable(*clients[i], closeClient);
                if (closeClient) {
                    closeSocket(clients[i]->sock);
                    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        }
    }
}

// ===========================================================================
// Encapsulation layer
// ===========================================================================

void Adapter::Impl::handleTcpReadable(TcpClient& c, bool& closeClient)
{
    uint8_t buf[4096];
    int n = recvBytes(c.sock, buf, sizeof buf);
    if (n <= 0) {
        closeClient = true;
        return;
    }
    c.rx.insert(c.rx.end(), buf, buf + n);

    while (c.rx.size() >= encap::kHeaderSize) {
        size_t total = encap::kHeaderSize + (size_t(c.rx[2]) | (size_t(c.rx[3]) << 8));
        if (total > kMaxEncapPacket) {
            closeClient = true;
            return;
        }
        if (c.rx.size() < total)
            break;
        auto reply = handleEncap(c.rx.data(), total, &c, c.peer, c.localIp, closeClient);
        c.rx.erase(c.rx.begin(), c.rx.begin() + static_cast<std::ptrdiff_t>(total));
        if (reply && !sendAll(c.sock, reply->data(), reply->size()))
            closeClient = true;
        if (closeClient)
            return;
    }
}

std::optional<std::vector<uint8_t>> Adapter::Impl::handleEncap(const uint8_t* pkt, size_t n, TcpClient* client,
                                                               const sockaddr_in& peer, in_addr localIp,
                                                               bool& closeClient)
{
    ByteReader hdr(pkt, n);
    uint16_t command = hdr.u16();
    uint16_t length = hdr.u16();
    uint32_t session = hdr.u32();
    hdr.u32(); // status
    const uint8_t* context = hdr.take(8);
    hdr.u32(); // options
    ByteReader body(pkt + encap::kHeaderSize, length);

    uint32_t status = encap::kStatusSuccess;
    uint32_t replySession = session;
    ByteWriter data;

    try {
        switch (command) {
        case encap::kNop:
            return std::nullopt;

        case encap::kListIdentity:
            data.u16(1);
            writeIdentityItem(data, localIp);
            break;

        case encap::kListServices:
            data.u16(1);
            writeServicesItem(data);
            break;

        case encap::kListInterfaces:
            data.u16(0);
            break;

        case encap::kRegisterSession: {
            if (!client)
                return std::nullopt;
            uint16_t version = body.u16();
            uint16_t options = body.u16();
            data.u16(1);
            data.u16(options);
            if (version != 1) {
                status = encap::kStatusUnsupportedProtocol;
            } else if (client->session != 0) {
                status = encap::kStatusInvalidCommand;
            } else {
                client->session = nextSession++;
                if (nextSession == 0)
                    nextSession = 1;
                replySession = client->session;
            }
            break;
        }

        case encap::kUnRegisterSession:
            closeClient = true;
            return std::nullopt;

        case encap::kSendRRData:
            if (!client)
                return std::nullopt;
            if (client->session == 0 || session != client->session) {
                status = encap::kStatusInvalidSession;
                break;
            }
            data.bytes(handleSendRRData(body, peer, status));
            break;

        default:
            if (!client)
                return std::nullopt;
            status = encap::kStatusInvalidCommand;
            break;
        }
    } catch (const ParseError&) {
        status = encap::kStatusInvalidLength;
        data.data().clear();
    }

    ByteWriter w;
    w.u16(command);
    w.u16(uint16_t(data.size()));
    w.u32(replySession);
    w.u32(status);
    w.bytes(context, 8);
    w.u32(0);
    w.bytes(data.data());
    return std::move(w.data());
}

void Adapter::Impl::writeIdentityItem(ByteWriter& w, in_addr localIp)
{
    const auto& id = cfg.identity;
    w.u16(cpf::kListIdentity);
    size_t lenPos = w.size();
    w.u16(0);
    size_t start = w.size();

    w.u16(1); // encapsulation protocol version
    // sockaddr_in, big-endian
    w.u16be(AF_INET);
    w.u16be(kEncapPort);
    w.bytes(&localIp, 4); // already network order
    w.zeros(8);

    w.u16(id.vendorId);
    w.u16(id.deviceType);
    w.u16(id.productCode);
    w.u8(id.revisionMajor);
    w.u8(id.revisionMinor);
    w.u16(identityStatus());
    w.u32(id.serialNumber);
    std::string name = id.productName.substr(0, 32);
    w.u8(uint8_t(name.size()));
    w.bytes(name.data(), name.size());
    w.u8(0x03); // state: operational

    w.patchU16(lenPos, uint16_t(w.size() - start));
}

void Adapter::Impl::writeServicesItem(ByteWriter& w)
{
    w.u16(cpf::kListServices);
    w.u16(20);
    w.u16(1);      // protocol version
    w.u16(0x0120); // bit 5: CIP over TCP, bit 8: class 0/1 over UDP
    char name[16] = "Communications";
    w.bytes(name, sizeof name);
}

in_addr Adapter::Impl::localIpFor(const sockaddr_in& peer) const
{
    if (bindIp.s_addr != htonl(INADDR_ANY))
        return bindIp;
    // Let the routing table pick the interface that would reach the peer.
    in_addr result{};
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket)
        return result;
    sockaddr_in to = peer;
    if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof local;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            result = local.sin_addr;
    }
    closeSocket(s);
    return result;
}

void Adapter::Impl::handleUdpEncap()
{
    uint8_t buf[1500];
    sockaddr_in peer{};
    int n = recvFrom(udpEncap, buf, sizeof buf, peer);
    if (n < int(encap::kHeaderSize))
        return;
    size_t total = encap::kHeaderSize + (size_t(buf[2]) | (size_t(buf[3]) << 8));
    if (total > size_t(n))
        return;
    bool unused = false;
    auto reply = handleEncap(buf, total, nullptr, peer, localIpFor(peer), unused);
    if (reply)
        sendTo(udpEncap, reply->data(), reply->size(), peer);
}

std::vector<uint8_t> Adapter::Impl::handleSendRRData(ByteReader& r, const sockaddr_in& peer, uint32_t& status)
{
    r.u32(); // interface handle (0 = CIP)
    r.u16(); // timeout
    uint16_t count = r.u16();

    RequestContext ctx;
    ctx.peer = peer;
    const uint8_t* msg = nullptr;
    size_t msgLen = 0;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t type = r.u16();
        uint16_t len = r.u16();
        const uint8_t* item = r.take(len);
        if (type == cpf::kUnconnectedData) {
            msg = item;
            msgLen = len;
        } else if (type == cpf::kSockaddrT2O && len >= 16) {
            ctx.t2oPort = uint16_t((item[2] << 8) | item[3]);
        }
    }
    if (!msg) {
        status = encap::kStatusIncorrectData;
        return {};
    }

    std::vector<uint8_t> resp = handleCip(msg, msgLen, ctx);

    ByteWriter w;
    w.u32(0);
    w.u16(0);
    w.u16(2);
    w.u16(cpf::kNullAddress);
    w.u16(0);
    w.u16(cpf::kUnconnectedData);
    w.u16(uint16_t(resp.size()));
    w.bytes(resp);
    return std::move(w.data());
}

// ===========================================================================
// CIP objects
// ===========================================================================

std::vector<uint8_t> Adapter::Impl::handleCip(const uint8_t* msg, size_t n, RequestContext& ctx, int depth)
{
    uint8_t service = 0;
    try {
        ByteReader r(msg, n);
        service = r.u8();
        size_t pathWords = r.u8();
        const uint8_t* pathBytes = r.take(pathWords * 2);

        ParsedPath path;
        if (!parsePath(pathBytes, pathWords * 2, path))
            return cipReply(service, cip::kPathSegmentError);
        if (!path.classId)
            return cipReply(service, cip::kPathDestinationUnknown);

        switch (*path.classId) {
        case cip::kIdentityClass:
            return handleIdentity(service, path);
        case cip::kAssemblyClass:
            return handleAssembly(service, path);
        case cip::kConnectionManagerClass:
            return handleConnectionManager(service, r, ctx, depth);
        default:
            return cipReply(service, cip::kPathDestinationUnknown);
        }
    } catch (const ParseError&) {
        return cipReply(service, cip::kNotEnoughData);
    }
}

uint16_t Adapter::Impl::identityStatus() const
{
    uint16_t status = 0x0004; // configured
    if (ownerConnected) {
        status |= 0x0001;                    // owned
        status |= plcRun ? 0x0060 : 0x0070;  // I/O connection in run / idle
    } else {
        status |= 0x0030; // no I/O connection established
    }
    return status;
}

std::vector<uint8_t> Adapter::Impl::handleIdentity(uint8_t service, const ParsedPath& path)
{
    if (path.instance.value_or(0) != 1)
        return cipReply(service, cip::kObjectDoesNotExist);

    const auto& id = cfg.identity;
    std::string name = id.productName.substr(0, 32);
    auto writeAttr = [&](ByteWriter& w, uint16_t attr) {
        switch (attr) {
        case 1: w.u16(id.vendorId); return true;
        case 2: w.u16(id.deviceType); return true;
        case 3: w.u16(id.productCode); return true;
        case 4: w.u8(id.revisionMajor); w.u8(id.revisionMinor); return true;
        case 5: w.u16(identityStatus()); return true;
        case 6: w.u32(id.serialNumber); return true;
        case 7: w.u8(uint8_t(name.size())); w.bytes(name.data(), name.size()); return true;
        default: return false;
        }
    };

    ByteWriter w;
    switch (service) {
    case cip::kGetAttributesAll:
        for (uint16_t a = 1; a <= 7; ++a)
            writeAttr(w, a);
        return cipReply(service, cip::kSuccess, w.data());
    case cip::kGetAttributeSingle:
        if (!path.attribute || !writeAttr(w, *path.attribute))
            return cipReply(service, cip::kAttributeNotSupported);
        return cipReply(service, cip::kSuccess, w.data());
    case cip::kReset:
        log("Identity reset requested (ignored)");
        return cipReply(service, cip::kSuccess);
    default:
        return cipReply(service, cip::kServiceNotSupported);
    }
}

std::vector<uint8_t> Adapter::Impl::handleAssembly(uint8_t service, const ParsedPath& path)
{
    uint16_t inst = path.instance.value_or(0);
    std::vector<uint8_t> image;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (inst == cfg.inputInstance)
            image = inputData;
        else if (inst == cfg.outputInstance)
            image = outputData;
        else if (inst != cfg.configInstance)
            return cipReply(service, cip::kObjectDoesNotExist);
    }

    if (service == cip::kSetAttributeSingle)
        return cipReply(service, cip::kAttributeNotSettable);
    if (service != cip::kGetAttributeSingle)
        return cipReply(service, cip::kServiceNotSupported);

    ByteWriter w;
    switch (path.attribute.value_or(0)) {
    case 3: w.bytes(image); break;
    case 4: w.u16(uint16_t(image.size())); break;
    default: return cipReply(service, cip::kAttributeNotSupported);
    }
    return cipReply(service, cip::kSuccess, w.data());
}

std::vector<uint8_t> Adapter::Impl::handleConnectionManager(uint8_t service, ByteReader& /*body*/,
                                                            RequestContext& /*ctx*/, int /*depth*/)
{
    // Forward_Open / Forward_Close / Unconnected_Send: phase 2.
    return cipReply(service, cip::kServiceNotSupported);
}

// ===========================================================================
// Public API
// ===========================================================================

Adapter::Adapter(AdapterConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Adapter::~Adapter() = default;

bool Adapter::start(std::string* error) { return impl_->start(error); }
void Adapter::stop() { impl_->stop(); }

void Adapter::setInputData(const uint8_t* data, size_t size)
{
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    auto& in = impl_->inputData;
    std::memcpy(in.data(), data, std::min(size, in.size()));
}

std::vector<uint8_t> Adapter::outputData() const
{
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    return impl_->outputData;
}

bool Adapter::plcInRun() const { return impl_->plcRun; }
bool Adapter::outputConnected() const { return impl_->ownerConnected; }

} // namespace softeip
