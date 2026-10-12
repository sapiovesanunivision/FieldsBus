// Software EtherNet/IP scanner (originator): one class-1 exclusive-owner connection to an adapter.
#include "softeip/eip_scanner.hpp"

#include "softeip/bytes.hpp"
#include "softeip/cip_defs.hpp"
#include "softeip/socket_compat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

namespace softeip {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kOrigVendor = 0xFFFE;
constexpr uint32_t kOrigSerial = 0x51A11234;
const std::vector<uint8_t> kConnMgrPath = {0x20, 0x06, 0x24, 0x01};

struct EncapReply {
    uint32_t status = 0;
    uint32_t session = 0;
    std::vector<uint8_t> data;
};

std::vector<uint8_t> encapPacket(uint16_t cmd, uint32_t session, const std::vector<uint8_t>& data)
{
    ByteWriter w;
    w.u16(cmd);
    w.u16(uint16_t(data.size()));
    w.u32(session);
    w.u32(0);
    w.bytes("softscan", 8);
    w.u32(0);
    w.bytes(data);
    return std::move(w.data());
}

bool recvExact(socket_t s, uint8_t* p, size_t n)
{
    while (n > 0) {
        int r = recvBytes(s, p, n);
        if (r <= 0)
            return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

std::optional<EncapReply> transact(socket_t s, uint16_t cmd, uint32_t session, const std::vector<uint8_t>& data)
{
    auto pkt = encapPacket(cmd, session, data);
    if (!sendAll(s, pkt.data(), pkt.size()))
        return std::nullopt;
    uint8_t hdr[24];
    if (!recvExact(s, hdr, sizeof hdr))
        return std::nullopt;
    ByteReader r(hdr, sizeof hdr);
    r.u16();
    const uint16_t len = r.u16();
    EncapReply reply;
    reply.session = r.u32();
    reply.status = r.u32();
    reply.data.resize(len);
    if (len && !recvExact(s, reply.data.data(), len))
        return std::nullopt;
    return reply;
}

// CIP request with SendRRData; returns the CIP reply (0xB2 item).
std::optional<std::vector<uint8_t>> cipRequest(socket_t s, uint32_t session, const std::vector<uint8_t>& cip,
                                               std::optional<uint16_t> t2oPort = std::nullopt)
{
    ByteWriter w;
    w.u32(0);
    w.u16(10);
    w.u16(t2oPort ? 3 : 2);
    w.u16(cpf::kNullAddress);
    w.u16(0);
    w.u16(cpf::kUnconnectedData);
    w.u16(uint16_t(cip.size()));
    w.bytes(cip);
    if (t2oPort) {
        w.u16(cpf::kSockaddrT2O);
        w.u16(16);
        w.u16be(AF_INET);
        w.u16be(*t2oPort);
        w.zeros(4 + 8);
    }
    auto reply = transact(s, encap::kSendRRData, session, w.data());
    if (!reply || reply->status != 0)
        return std::nullopt;
    ByteReader r(reply->data.data(), reply->data.size());
    r.u32();
    r.u16();
    const uint16_t count = r.u16();
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t type = r.u16();
        const uint16_t len = r.u16();
        const uint8_t* item = r.take(len);
        if (type == cpf::kUnconnectedData)
            return std::vector<uint8_t>(item, item + len);
    }
    return std::nullopt;
}

// General status; fills the extended status and the reply data.
uint8_t parseCipReply(const std::vector<uint8_t>& reply, uint16_t& ext, std::vector<uint8_t>& data)
{
    ByteReader r(reply.data(), reply.size());
    r.u8();
    r.u8();
    const uint8_t status = r.u8();
    const uint8_t extWords = r.u8();
    ext = extWords ? r.u16() : 0;
    if (extWords > 1)
        r.skip((extWords - 1) * 2u);
    data.assign(r.cur(), r.cur() + r.remaining());
    return status;
}

void appendLogical(std::vector<uint8_t>& p, uint8_t type8, uint16_t value)
{
    if (value <= 0xFF) {
        p.push_back(type8);
        p.push_back(uint8_t(value));
    } else {
        p.push_back(uint8_t(type8 | 1));
        p.push_back(0);
        p.push_back(uint8_t(value));
        p.push_back(uint8_t(value >> 8));
    }
}

std::string hex(unsigned v, int digits)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*X", digits, v);
    return buf;
}

} // namespace

struct Scanner::Impl {
    explicit Impl(ScannerConfig c) : cfg(std::move(c)), outputs(cfg.outputSize, 0), inputs(cfg.inputSize, 0) {}
    ~Impl() { stop(true); }

    bool start(std::string* error);
    void stop(bool forwardClose);
    void run();
    bool connect(); // ListIdentity, session, Forward_Open, UDP socket
    void cyclic();  // until stop or connection loss
    void disconnect(bool forwardClose);
    void setConnected(bool up);
    bool sleepMs(unsigned ms); // false when stopping

    void log(const std::string& m) const
    {
        if (cfg.onLog)
            cfg.onLog(m);
    }
    void fail(const std::string& m)
    {
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            stats.lastError = m;
        }
        log(m);
    }

    ScannerConfig cfg;
    SocketLibrary socketLib;
    sockaddr_in target{};
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> up{false};
    std::atomic<bool> runMode{true}; // run/idle bit
    bool closeOnStop = true;

    mutable std::mutex dataMutex;
    std::vector<uint8_t> outputs; // O->T
    std::vector<uint8_t> inputs;  // T->O

    mutable std::mutex statsMutex;
    Stats stats;

    // connection, network thread only
    socket_t tcp = kInvalidSocket;
    socket_t udp = kInvalidSocket;
    uint32_t session = 0;
    uint32_t o2tId = 0, t2oId = 0;
    uint16_t connSerial = 0;
    std::vector<uint8_t> connPath;
    std::mt19937 rng{std::random_device{}()};
};

bool Scanner::Impl::start(std::string* error)
{
    if (running)
        return true;
    auto bad = [&](const std::string& m) {
        if (error)
            *error = m;
        return false;
    };
    target = {};
    target.sin_family = AF_INET;
    if (inet_pton(AF_INET, cfg.target.c_str(), &target.sin_addr) != 1)
        return bad("bad target address " + cfg.target);
    if (cfg.rpiUs < 1000)
        return bad("rpiUs must be >= 1000");
    if (cfg.outputSize + 6 > 0x1FF || cfg.inputSize + 2 > 0x1FF)
        return bad("I/O size above 505 / 509 bytes needs a Large Forward_Open (not supported)");
    running = true;
    closeOnStop = true;
    thread = std::thread([this] { run(); });
    return true;
}

void Scanner::Impl::stop(bool forwardClose)
{
    closeOnStop = forwardClose;
    running = false;
    if (thread.joinable())
        thread.join();
}

bool Scanner::Impl::sleepMs(unsigned ms)
{
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (running && Clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return running;
}

void Scanner::Impl::setConnected(bool nowUp)
{
    if (up == nowUp)
        return;
    up = nowUp;
    if (cfg.onConnectionChanged)
        cfg.onConnectionChanged(nowUp);
}

void Scanner::Impl::run()
{
    while (running) {
        bool ok = false;
        try {
            ok = connect();
        } catch (const std::exception& e) {
            fail(std::string("malformed reply from the adapter: ") + e.what());
        }
        if (ok) {
            cyclic();
            disconnect(running ? false : closeOnStop); // lost: the connection is gone anyway
        } else {
            disconnect(false);
        }
        if (running)
            sleepMs(cfg.reconnectDelayMs);
    }
}

bool Scanner::Impl::connect()
{
    // ---- ListIdentity (UDP, informational) ----
    {
        socket_t u = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (u != kInvalidSocket) {
            setRecvTimeoutMs(u, 500);
            sockaddr_in to = target;
            to.sin_port = htons(kEncapPort);
            auto pkt = encapPacket(encap::kListIdentity, 0, {});
            sendTo(u, pkt.data(), pkt.size(), to);
            uint8_t buf[600];
            sockaddr_in from{};
            const int n = recvFrom(u, buf, sizeof buf, from);
            closeSocket(u);
            if (n >= 24 + 2 + 4 + 2 + 16 + 14 + 1) {
                ByteReader r(buf + 24, size_t(n) - 24);
                r.u16(); // count
                r.u16(); // type
                r.u16(); // len
                r.u16(); // version
                r.skip(16);
                IdentityInfo id;
                id.vendorId = r.u16();
                id.deviceType = r.u16();
                id.productCode = r.u16();
                id.revisionMajor = r.u8();
                id.revisionMinor = r.u8();
                r.u16(); // status
                id.serialNumber = r.u32();
                const uint8_t nameLen = r.u8();
                id.productName.assign(reinterpret_cast<const char*>(r.take(nameLen)), nameLen);
                log("ListIdentity: \"" + id.productName + "\" vendor " + std::to_string(id.vendorId) + " type " +
                    std::to_string(id.deviceType) + " product " + std::to_string(id.productCode) + " rev " +
                    std::to_string(id.revisionMajor) + "." + std::to_string(id.revisionMinor) + " serial " +
                    hex(id.serialNumber, 8));
                std::lock_guard<std::mutex> lock(statsMutex);
                stats.identity = id;
                stats.identityValid = true;
            }
        }
    }

    // ---- TCP session ----
    tcp = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in to = target;
    to.sin_port = htons(kEncapPort);
    if (tcp == kInvalidSocket || !connectWithTimeout(tcp, to, cfg.tcpTimeoutMs)) {
        fail("TCP connect to " + cfg.target + ":44818 failed");
        return false;
    }
    setNoDelay(tcp);
    setRecvTimeoutMs(tcp, cfg.tcpTimeoutMs);
    auto reg = transact(tcp, encap::kRegisterSession, 0, {1, 0, 0, 0});
    if (!reg || reg->status != 0) {
        fail("RegisterSession failed");
        return false;
    }
    session = reg->session;
    log("RegisterSession: handle 0x" + hex(session, 8));

    // ---- Identity product name (Get_Attribute_Single, informational) ----
    {
        auto reply = cipRequest(tcp, session, {cip::kGetAttributeSingle, 3, 0x20, 0x01, 0x24, 0x01, 0x30, 0x07});
        uint16_t ext = 0;
        std::vector<uint8_t> data;
        if (reply && parseCipReply(*reply, ext, data) == 0 && !data.empty() && data.size() > data[0]) {
            const std::string name(reinterpret_cast<const char*>(&data[1]), data[0]);
            log("Identity.ProductName = \"" + name + "\"");
            std::lock_guard<std::mutex> lock(statsMutex);
            stats.productName = name;
        }
    }

    // ---- Forward_Open ----
    connSerial = uint16_t(rng());
    connPath = {0x20, 0x04};
    appendLogical(connPath, 0x24, cfg.configInstance);
    appendLogical(connPath, 0x2C, cfg.o2tInstance);
    appendLogical(connPath, 0x2C, cfg.t2oInstance);

    ByteWriter fo;
    fo.u8(cip::kForwardOpen);
    fo.u8(2);
    fo.bytes(kConnMgrPath);
    fo.u8(0x0A);   // priority / time tick
    fo.u8(0x0E);   // timeout ticks
    fo.u32(0);     // O->T ID: chosen by the target
    fo.u32(rng()); // T->O ID proposal
    fo.u16(connSerial);
    fo.u16(kOrigVendor);
    fo.u32(kOrigSerial);
    fo.u8(1); // timeout multiplier: x8
    fo.zeros(3);
    fo.u32(cfg.rpiUs);
    fo.u16(uint16_t(0x4800 | ((cfg.outputSize + 6) & 0x1FF))); // P2P, scheduled, fixed; seq + run/idle header
    fo.u32(cfg.rpiUs);
    fo.u16(uint16_t(0x4800 | ((cfg.inputSize + 2) & 0x1FF))); // + sequence count
    fo.u8(0x01); // class 1, cyclic
    fo.u8(uint8_t(connPath.size() / 2));
    fo.bytes(connPath);

    auto foReply = cipRequest(tcp, session, fo.data(), cfg.localIoPort);
    if (!foReply) {
        fail("Forward_Open: no reply");
        return false;
    }
    uint16_t ext = 0;
    std::vector<uint8_t> foData;
    const uint8_t st = parseCipReply(*foReply, ext, foData);
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        stats.lastOpenStatus = st;
        stats.lastOpenExtStatus = ext;
    }
    if (st != 0) {
        fail("Forward_Open REJECTED: general 0x" + hex(st, 2) + " extended 0x" + hex(ext, 4));
        return false;
    }
    ByteReader fr(foData.data(), foData.size());
    o2tId = fr.u32();
    t2oId = fr.u32();
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++stats.connects;
        stats.o2tId = o2tId;
        stats.t2oId = t2oId;
    }
    log("Forward_Open OK: O->T id 0x" + hex(o2tId, 8) + ", T->O id 0x" + hex(t2oId, 8) + ", RPI " +
        std::to_string(cfg.rpiUs / 1000) + " ms");

    // ---- UDP I/O socket ----
    udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == kInvalidSocket) {
        fail("UDP socket failed");
        return false;
    }
    setReuseAddr(udp);
    disableUdpConnReset(udp);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(cfg.localIoPort);
    if (::bind(udp, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        fail("cannot bind UDP " + std::to_string(cfg.localIoPort));
        return false;
    }
    return true;
}

void Scanner::Impl::cyclic()
{
    sockaddr_in ioTarget = target;
    ioTarget.sin_port = htons(kIoPort);
    const auto rpi = std::chrono::microseconds(cfg.rpiUs);
    // Connection lost after 8 RPIs without T->O data (the Forward_Open timeout multiplier), at least 100 ms;
    // the first packet may take up to 1 s.
    const auto lossTimeout = std::max<Clock::duration>(rpi * 8, std::chrono::milliseconds(100));
    auto nextSend = Clock::now();
    auto lastRx = Clock::now() + std::chrono::seconds(1) - lossTimeout;
    uint32_t encapSeq = 0;
    uint16_t cipSeq = 0;
    std::vector<uint8_t> out;
    std::vector<uint8_t> in(cfg.inputSize);

    while (running) {
        const auto now = Clock::now();
        if (now >= nextSend) {
            {
                std::lock_guard<std::mutex> lock(dataMutex);
                out = outputs;
            }
            ByteWriter w;
            w.u16(2);
            w.u16(cpf::kSequencedAddress);
            w.u16(8);
            w.u32(o2tId);
            w.u32(++encapSeq);
            w.u16(cpf::kConnectedData);
            w.u16(uint16_t(2 + 4 + out.size()));
            w.u16(++cipSeq);
            w.u32(runMode ? 1u : 0u); // run/idle header
            w.bytes(out);
            sendTo(udp, w.data().data(), w.size(), ioTarget);
            {
                std::lock_guard<std::mutex> lock(statsMutex);
                ++stats.sent;
            }
            nextSend += rpi;
            if (nextSend < now)
                nextSend = now + rpi; // late (debugger, suspended): do not burst
        }

        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(nextSend - Clock::now()).count();
        if (wait < 0)
            wait = 0;
        if (waitReadable(udp, unsigned(wait)) > 0) {
            uint8_t buf[1500];
            sockaddr_in from{};
            const int n = recvFrom(udp, buf, sizeof buf, from);
            if (n >= 2 + 4 + 8 + 4 + 2) {
                try {
                    ByteReader r(buf, size_t(n));
                    r.u16();
                    r.u16();
                    r.u16();
                    const uint32_t id = r.u32();
                    r.u32();
                    r.u16();
                    const uint16_t len = r.u16();
                    if (id == t2oId && len == cfg.inputSize + 2 && r.remaining() >= len) {
                        r.u16(); // sequence count
                        std::memcpy(in.data(), r.take(cfg.inputSize), cfg.inputSize);
                        {
                            std::lock_guard<std::mutex> lock(dataMutex);
                            inputs = in;
                        }
                        {
                            std::lock_guard<std::mutex> lock(statsMutex);
                            ++stats.received;
                        }
                        lastRx = Clock::now();
                        setConnected(true);
                        if (cfg.onInputs)
                            cfg.onInputs(in.data(), in.size());
                    }
                } catch (const std::exception&) {
                    // malformed datagram: ignore
                }
            }
        }

        if (Clock::now() - lastRx > lossTimeout) {
            fail("connection lost: no T->O data for " +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(lossTimeout).count()) + " ms");
            return;
        }
    }
}

void Scanner::Impl::disconnect(bool forwardClose)
{
    if (forwardClose && tcp != kInvalidSocket && session != 0) {
        bool ok = false;
        try {
            ByteWriter fc;
            fc.u8(cip::kForwardClose);
            fc.u8(2);
            fc.bytes(kConnMgrPath);
            fc.u8(0x0A);
            fc.u8(0x0E);
            fc.u16(connSerial);
            fc.u16(kOrigVendor);
            fc.u32(kOrigSerial);
            fc.u8(uint8_t(connPath.size() / 2));
            fc.u8(0);
            fc.bytes(connPath);
            auto reply = cipRequest(tcp, session, fc.data());
            uint16_t ext = 0;
            std::vector<uint8_t> d;
            ok = reply && parseCipReply(*reply, ext, d) == 0;
        } catch (const std::exception&) {
            ok = false;
        }
        log(std::string("Forward_Close: ") + (ok ? "OK" : "FAILED"));
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            stats.forwardCloseOk = ok;
        }
        auto unreg = encapPacket(encap::kUnRegisterSession, session, {});
        sendAll(tcp, unreg.data(), unreg.size());
    }
    if (tcp != kInvalidSocket)
        closeSocket(tcp);
    if (udp != kInvalidSocket)
        closeSocket(udp);
    tcp = udp = kInvalidSocket;
    session = 0;
    setConnected(false);
}

// ===========================================================================
// Public API
// ===========================================================================

Scanner::Scanner(ScannerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Scanner::~Scanner() = default;

bool Scanner::start(std::string* error) { return impl_->start(error); }
void Scanner::stop(bool forwardClose) { impl_->stop(forwardClose); }

bool Scanner::ioWrite(size_t offset, const void* data, size_t len)
{
    if (offset > impl_->cfg.outputSize || len > impl_->cfg.outputSize - offset)
        return false;
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    std::memcpy(impl_->outputs.data() + offset, data, len);
    return true;
}

bool Scanner::ioRead(size_t offset, void* data, size_t len) const
{
    if (offset > impl_->cfg.inputSize || len > impl_->cfg.inputSize - offset)
        return false;
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    std::memcpy(data, impl_->inputs.data() + offset, len);
    return true;
}

void Scanner::setRun(bool run) { impl_->runMode = run; }
bool Scanner::connected() const { return impl_->up; }

Scanner::Stats Scanner::stats() const
{
    std::lock_guard<std::mutex> lock(impl_->statsMutex);
    return impl_->stats;
}

} // namespace softeip
