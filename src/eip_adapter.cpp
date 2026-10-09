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

std::string hex16(uint16_t v)
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

// Network connection parameters of a Forward_Open (normal: 16 bit, large: 32 bit).
struct NetParams {
    enum Type { Null = 0, Multicast = 1, PointToPoint = 2 };
    Type type = Null;
    bool variable = false;
    size_t size = 0;

    static NetParams decode(uint32_t raw, bool large)
    {
        NetParams p;
        if (large) {
            p.type = Type((raw >> 29) & 3);
            p.variable = (raw >> 25) & 1;
            p.size = raw & 0xFFFF;
        } else {
            p.type = Type((raw >> 13) & 3);
            p.variable = (raw >> 9) & 1;
            p.size = raw & 0x1FF;
        }
        return p;
    }
};

struct IoConnection {
    enum class Kind { ExclusiveOwner, InputOnly, ListenOnly };
    Kind kind = Kind::ExclusiveOwner;

    // Connection triad
    uint16_t serial = 0;
    uint16_t originatorVendor = 0;
    uint32_t originatorSerial = 0;

    uint32_t o2tId = 0; // we consume packets carrying this ID
    uint32_t t2oId = 0; // we produce packets carrying this ID
    uint32_t t2oRpiUs = 0;
    uint64_t timeoutUs = 0; // 0 = no consumption watchdog (O->T null)
    bool o2tHasHeader = false;

    in_addr originator{};
    sockaddr_in t2oDest{};

    Clock::time_point nextProduce;
    Clock::time_point lastConsumed;
    bool consumedAny = false;
    uint32_t encapSeq = 0;
    uint16_t cipSeq = 0;
    uint16_t lastO2tSeq = 0;

    const char* kindName() const
    {
        switch (kind) {
        case Kind::ExclusiveOwner: return "exclusive-owner";
        case Kind::InputOnly: return "input-only";
        default: return "listen-only";
        }
    }
};

constexpr size_t kMaxIoConnections = 8;
constexpr auto kInitialWatchdog = std::chrono::seconds(10);

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
    std::vector<uint8_t> forwardOpen(uint8_t service, ByteReader& r, const RequestContext& ctx);
    std::vector<uint8_t> forwardClose(uint8_t service, ByteReader& r);

    // ---- class-1 I/O ------------------------------------------------------

    void handleIoPacket();
    void produce(IoConnection& c, const std::vector<uint8_t>& image);
    void serviceConnections(Clock::time_point now, Clock::time_point& nextEvent);
    void closeConnection(size_t index, const char* reason);
    void updateOwnerState();

    // ---- helpers ----------------------------------------------------------

    std::vector<uint8_t> outputDataSnapshot() const
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        return outputData;
    }

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
    std::vector<IoConnection> connections; // network thread only

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
    connections.clear();
    ownerConnected = false;
    plcRun = false;
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
        auto now = Clock::now();
        auto nextEvent = now + std::chrono::milliseconds(50);
        serviceConnections(now, nextEvent);
        auto timeout = std::chrono::duration_cast<std::chrono::microseconds>(nextEvent - Clock::now());
        if (timeout.count() < 0)
            timeout = std::chrono::microseconds(0);

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
            if (FD_ISSET(udpIo, &readSet))
                handleIoPacket();
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

std::vector<uint8_t> Adapter::Impl::handleConnectionManager(uint8_t service, ByteReader& body,
                                                            RequestContext& ctx, int depth)
{
    switch (service) {
    case cip::kForwardOpen:
    case cip::kLargeForwardOpen:
        return forwardOpen(service, body, ctx);
    case cip::kForwardClose:
        return forwardClose(service, body);
    case cip::kUnconnectedSend: {
        // We are the end node: just execute the embedded request. Its reply
        // is returned as-is (that is the Unconnected_Send success reply).
        if (depth > 1)
            return cipReply(service, cip::kPathSegmentError);
        body.u8(); // priority / time tick
        body.u8(); // timeout ticks
        uint16_t size = body.u16();
        const uint8_t* embedded = body.take(size);
        return handleCip(embedded, size, ctx, depth + 1);
    }
    default:
        return cipReply(service, cip::kServiceNotSupported);
    }
}

std::vector<uint8_t> Adapter::Impl::forwardOpen(uint8_t service, ByteReader& r, const RequestContext& ctx)
{
    const bool large = service == cip::kLargeForwardOpen;
    r.u8(); // priority / time tick
    r.u8(); // timeout ticks
    r.u32(); // O->T connection ID proposal (target chooses for point-to-point)
    uint32_t t2oIdProposed = r.u32();
    uint16_t serial = r.u16();
    uint16_t origVendor = r.u16();
    uint32_t origSerial = r.u32();
    uint8_t timeoutMultiplier = r.u8();
    r.skip(3);
    uint32_t o2tRpi = r.u32();
    NetParams o2t = NetParams::decode(large ? r.u32() : r.u16(), large);
    uint32_t t2oRpi = r.u32();
    NetParams t2o = NetParams::decode(large ? r.u32() : r.u16(), large);
    uint8_t transport = r.u8();
    size_t pathWords = r.u8();
    const uint8_t* pathBytes = r.take(pathWords * 2);

    auto fail = [&](uint16_t ext, const std::string& why) {
        log("Forward_Open from " + ipToString(ctx.peer.sin_addr) + " rejected (" + hex16(ext) + "): " + why);
        ByteWriter w;
        w.u16(serial);
        w.u16(origVendor);
        w.u32(origSerial);
        w.u8(0); // remaining path size
        w.u8(0);
        return cipReply(service, cip::kConnectionFailure, w.data(), {ext});
    };

    ParsedPath path;
    if (!parsePath(pathBytes, pathWords * 2, path) || path.classId.value_or(0) != cip::kAssemblyClass)
        return fail(cip::cm::kInvalidSegmentInPath, "connection path");

    // Electronic key (0 fields = don't care).
    if (path.hasKey) {
        const auto& id = cfg.identity;
        if ((path.keyVendor && path.keyVendor != id.vendorId) ||
            (path.keyProductCode && path.keyProductCode != id.productCode))
            return fail(cip::cm::kVendorOrProductMismatch, "electronic key vendor/product");
        if (path.keyDeviceType && path.keyDeviceType != id.deviceType)
            return fail(cip::cm::kDeviceTypeMismatch, "electronic key device type");
        bool compatible = (path.keyMajor & 0x80) != 0;
        uint8_t major = path.keyMajor & 0x7F;
        if (major != 0) {
            bool ok = major == id.revisionMajor &&
                      (path.keyMinor == 0 ||
                       (compatible ? path.keyMinor <= id.revisionMinor : path.keyMinor == id.revisionMinor));
            if (!ok)
                return fail(cip::cm::kRevisionMismatch, "electronic key revision");
        }
    }

    if ((transport & 0x0F) != 1)
        return fail(cip::cm::kTransportClassNotSupported, "only class 1 I/O is supported");

    // Map instance ids -> config / O->T point / T->O point.
    const auto& ids = path.instanceIds;
    std::optional<uint16_t> o2tPoint, t2oPoint;
    bool oneDirectionNull = o2t.type == NetParams::Null || t2o.type == NetParams::Null;
    auto assignSingle = [&](uint16_t v) {
        if (o2t.type == NetParams::Null)
            t2oPoint = v;
        else
            o2tPoint = v;
    };
    if (ids.size() >= 3) {
        o2tPoint = ids[1];
        t2oPoint = ids[2];
    } else if (ids.size() == 2) {
        if (oneDirectionNull)
            assignSingle(ids[1]); // ids[0] is the configuration instance
        else {
            o2tPoint = ids[0];
            t2oPoint = ids[1];
        }
    } else if (ids.size() == 1 && oneDirectionNull) {
        assignSingle(ids[0]);
    } else {
        return fail(cip::cm::kInvalidApplicationPath, "missing connection points");
    }

    IoConnection c;
    if (t2oPoint != cfg.inputInstance)
        return fail(cip::cm::kInvalidApplicationPath, "unknown T->O instance");
    if (o2tPoint == cfg.outputInstance)
        c.kind = IoConnection::Kind::ExclusiveOwner;
    else if (o2tPoint == cfg.listenOnlyInstance)
        c.kind = IoConnection::Kind::ListenOnly;
    else if (o2tPoint == cfg.inputOnlyInstance || !o2tPoint)
        c.kind = IoConnection::Kind::InputOnly;
    else
        return fail(cip::cm::kInvalidApplicationPath, "unknown O->T instance");

    // Connection types: we produce unicast only, and consume unicast only.
    if (t2o.type != NetParams::PointToPoint)
        return fail(cip::cm::kInvalidT2OConnectionType, "T->O must be unicast (point-to-point)");
    if (c.kind == IoConnection::Kind::ExclusiveOwner && o2t.type != NetParams::PointToPoint)
        return fail(cip::cm::kInvalidO2TConnectionType, "O->T must be point-to-point");
    if (o2t.type == NetParams::Multicast)
        return fail(cip::cm::kInvalidO2TConnectionType, "O->T multicast not supported");

    // Sizes include the 2-byte sequence count (+ 4-byte run/idle header on O->T).
    if (t2o.size != cfg.inputSize + 2)
        return fail(cip::cm::kInvalidT2OConnectionSize,
                    "T->O size " + std::to_string(t2o.size) + ", expected " + std::to_string(cfg.inputSize + 2));
    if (c.kind == IoConnection::Kind::ExclusiveOwner) {
        if (o2t.size != cfg.outputSize + 6)
            return fail(cip::cm::kInvalidO2TConnectionSize,
                        "O->T size " + std::to_string(o2t.size) + ", expected " +
                            std::to_string(cfg.outputSize + 6));
        c.o2tHasHeader = true;
    } else {
        if (o2t.type != NetParams::Null && o2t.size > 6)
            return fail(cip::cm::kInvalidO2TConnectionSize, "heartbeat O->T too large");
        c.o2tHasHeader = o2t.size == 6;
    }

    if (t2oRpi < cfg.minRpiUs || (o2t.type != NetParams::Null && o2tRpi < cfg.minRpiUs))
        return fail(cip::cm::kRpiNotSupported, "RPI below " + std::to_string(cfg.minRpiUs) + " us");

    for (const auto& other : connections) {
        bool sameTriad = other.serial == serial && other.originatorVendor == origVendor &&
                         other.originatorSerial == origSerial;
        if (sameTriad)
            return fail(cip::cm::kDuplicateForwardOpen, "duplicate Forward_Open");
        if (c.kind == IoConnection::Kind::ExclusiveOwner && other.kind == IoConnection::Kind::ExclusiveOwner)
            return fail(cip::cm::kOwnershipConflict, "output already owned by " + ipToString(other.originator));
    }
    if (c.kind == IoConnection::Kind::ListenOnly &&
        std::none_of(connections.begin(), connections.end(),
                     [](const IoConnection& o) { return o.kind != IoConnection::Kind::ListenOnly; }))
        return fail(cip::cm::kNonListenOnlyNotOpened, "listen-only needs an open owner/input-only connection");
    if (connections.size() >= kMaxIoConnections)
        return fail(cip::cm::kOutOfConnections, "too many connections");

    c.serial = serial;
    c.originatorVendor = origVendor;
    c.originatorSerial = origSerial;
    c.o2tId = nextConnectionId++;
    c.t2oId = t2oIdProposed != 0 ? t2oIdProposed : nextConnectionId++;
    c.t2oRpiUs = t2oRpi;
    if (o2t.type != NetParams::Null)
        c.timeoutUs = uint64_t(o2tRpi) * (uint64_t(4) << std::min<uint8_t>(timeoutMultiplier, 7));
    c.originator = ctx.peer.sin_addr;
    c.t2oDest.sin_family = AF_INET;
    c.t2oDest.sin_addr = ctx.peer.sin_addr;
    c.t2oDest.sin_port = htons(ctx.t2oPort.value_or(kIoPort));
    auto now = Clock::now();
    c.nextProduce = now;
    c.lastConsumed = now;

    log(std::string("Forward_Open OK: ") + c.kindName() + " from " + ipToString(c.originator) +
        ", RPI O->T " + std::to_string(o2tRpi) + " us / T->O " + std::to_string(t2oRpi) +
        " us, T->O to port " + std::to_string(ntohs(c.t2oDest.sin_port)));

    ByteWriter w;
    w.u32(c.o2tId);
    w.u32(c.t2oId);
    w.u16(serial);
    w.u16(origVendor);
    w.u32(origSerial);
    w.u32(o2tRpi); // actual packet intervals = requested
    w.u32(t2oRpi);
    w.u8(0); // application reply size
    w.u8(0);

    connections.push_back(c);
    updateOwnerState();
    return cipReply(service, cip::kSuccess, w.data());
}

std::vector<uint8_t> Adapter::Impl::forwardClose(uint8_t service, ByteReader& r)
{
    r.u8();
    r.u8();
    uint16_t serial = r.u16();
    uint16_t origVendor = r.u16();
    uint32_t origSerial = r.u32();

    ByteWriter w;
    w.u16(serial);
    w.u16(origVendor);
    w.u32(origSerial);
    w.u8(0); // application reply size / remaining path size
    w.u8(0);

    for (size_t i = 0; i < connections.size(); ++i) {
        const auto& c = connections[i];
        if (c.serial == serial && c.originatorVendor == origVendor && c.originatorSerial == origSerial) {
            closeConnection(i, "Forward_Close");
            return cipReply(service, cip::kSuccess, w.data());
        }
    }
    return cipReply(service, cip::kConnectionFailure, w.data(), {cip::cm::kConnectionNotFound});
}

// ===========================================================================
// Class-1 implicit I/O
// ===========================================================================

void Adapter::Impl::handleIoPacket()
{
    uint8_t buf[1500];
    sockaddr_in from{};
    int n = recvFrom(udpIo, buf, sizeof buf, from);
    if (n <= 0)
        return;

    uint32_t connId = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    bool haveAddress = false;
    try {
        ByteReader r(buf, size_t(n));
        uint16_t count = r.u16();
        for (uint16_t i = 0; i < count; ++i) {
            uint16_t type = r.u16();
            uint16_t len = r.u16();
            const uint8_t* item = r.take(len);
            if (type == cpf::kSequencedAddress && len == 8) {
                ByteReader a(item, len);
                connId = a.u32();
                haveAddress = true;
            } else if (type == cpf::kConnectedData) {
                payload = item;
                payloadLen = len;
            }
        }
    } catch (const ParseError&) {
        return;
    }
    if (!haveAddress || !payload || payloadLen < 2)
        return;

    auto it = std::find_if(connections.begin(), connections.end(), [&](const IoConnection& c) {
        return c.o2tId == connId && c.originator.s_addr == from.sin_addr.s_addr;
    });
    if (it == connections.end())
        return;
    IoConnection& c = *it;

    c.lastConsumed = Clock::now();
    uint16_t seq = uint16_t(payload[0] | (payload[1] << 8));
    if (c.consumedAny && int16_t(seq - c.lastO2tSeq) <= 0)
        return; // duplicate / stale: watchdog refreshed, data ignored
    c.consumedAny = true;
    c.lastO2tSeq = seq;

    if (c.kind != IoConnection::Kind::ExclusiveOwner)
        return;

    const uint8_t* data = payload + 2;
    size_t dataLen = payloadLen - 2;
    bool runBit = true;
    if (c.o2tHasHeader) {
        if (dataLen < 4)
            return;
        runBit = (data[0] & 0x01) != 0;
        data += 4;
        dataLen -= 4;
    }
    if (dataLen != cfg.outputSize)
        return;

    bool changed;
    std::vector<uint8_t> snapshot;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        changed = runBit != plcRun || std::memcmp(outputData.data(), data, dataLen) != 0;
        if (changed) {
            std::memcpy(outputData.data(), data, dataLen);
            snapshot = outputData;
        }
    }
    if (runBit != plcRun) {
        plcRun = runBit;
        log(std::string("PLC switched to ") + (runBit ? "RUN" : "IDLE"));
    }
    if (changed && cfg.onOutputs)
        cfg.onOutputs(snapshot, runBit);
}

void Adapter::Impl::produce(IoConnection& c, const std::vector<uint8_t>& image)
{
    ByteWriter w;
    w.u16(2);
    w.u16(cpf::kSequencedAddress);
    w.u16(8);
    w.u32(c.t2oId);
    w.u32(++c.encapSeq);
    w.u16(cpf::kConnectedData);
    w.u16(uint16_t(image.size() + 2));
    w.u16(++c.cipSeq);
    w.bytes(image);
    sendTo(udpIo, w.data().data(), w.size(), c.t2oDest);
}

void Adapter::Impl::serviceConnections(Clock::time_point now, Clock::time_point& nextEvent)
{
    // Consumption watchdog
    for (size_t i = 0; i < connections.size();) {
        const auto& c = connections[i];
        if (c.timeoutUs > 0) {
            auto limit = std::chrono::microseconds(c.timeoutUs);
            if (!c.consumedAny)
                limit = std::max<std::chrono::microseconds>(limit, kInitialWatchdog);
            if (now - c.lastConsumed > limit) {
                closeConnection(i, "connection timed out");
                continue;
            }
            nextEvent = std::min(nextEvent, c.lastConsumed + limit);
        }
        ++i;
    }

    // Production
    std::vector<uint8_t> image;
    for (auto& c : connections) {
        if (now < c.nextProduce) {
            nextEvent = std::min(nextEvent, c.nextProduce);
            continue;
        }
        if (image.empty()) {
            std::lock_guard<std::mutex> lock(dataMutex);
            image = inputData;
        }
        produce(c, image);
        auto rpi = std::chrono::microseconds(c.t2oRpiUs);
        c.nextProduce += rpi;
        if (c.nextProduce <= now) // fell behind (e.g. OS hiccup): resync, don't burst
            c.nextProduce = now + rpi;
        nextEvent = std::min(nextEvent, c.nextProduce);
    }
}

void Adapter::Impl::closeConnection(size_t index, const char* reason)
{
    const IoConnection& c = connections[index];
    log(std::string(c.kindName()) + " connection from " + ipToString(c.originator) + " closed: " + reason);
    connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(index));

    // Listen-only connections cannot outlive the last non-listen-only one.
    bool anyNonListen = std::any_of(connections.begin(), connections.end(), [](const IoConnection& o) {
        return o.kind != IoConnection::Kind::ListenOnly;
    });
    if (!anyNonListen && !connections.empty()) {
        log("closing listen-only connections (no owner left)");
        connections.clear();
    }
    updateOwnerState();
}

void Adapter::Impl::updateOwnerState()
{
    bool owner = std::any_of(connections.begin(), connections.end(), [](const IoConnection& c) {
        return c.kind == IoConnection::Kind::ExclusiveOwner;
    });
    if (owner == ownerConnected)
        return;
    ownerConnected = owner;
    if (!owner && plcRun) {
        plcRun = false;
        if (cfg.onOutputs)
            cfg.onOutputs(outputDataSnapshot(), false);
    }
    if (cfg.onConnectionChanged)
        cfg.onConnectionChanged(owner);
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
