// Minimal EtherNet/IP *scanner* (PLC side) used to test the adapter without a
// real PLC. It does what a PLC does at connection time and then exchanges
// cyclic class-1 I/O for a while.
//
// Usage: eip_scanner_sim [--target IP] [--rpi-ms N] [--seconds N] [--local-port N]
//                        [--in-size N] [--out-size N] [--o2t N] [--t2o N] [--cfg N]
//                        [--skip-close 1]
//
// --local-port: UDP port we receive T->O on (announced with a Sockaddr Info
// item). Use something other than 2222 when the adapter runs on the same PC.
#include "softeip/bytes.hpp"
#include "softeip/cip_defs.hpp"
#include "softeip/socket_compat.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace softeip;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string target = "127.0.0.1";
    uint32_t rpiMs = 10;
    int seconds = 5;
    uint16_t localPort = 2223;
    uint16_t inSize = 32;
    uint16_t outSize = 32;
    uint16_t o2tInstance = 150;
    uint16_t t2oInstance = 100;
    uint16_t cfgInstance = 151;
    bool skipClose = false;
};

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
    w.bytes("simctx01", 8);
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
    uint16_t len = r.u16();
    EncapReply reply;
    reply.session = r.u32();
    reply.status = r.u32();
    reply.data.resize(len);
    if (len && !recvExact(s, reply.data.data(), len))
        return std::nullopt;
    return reply;
}

// Sends a CIP request with SendRRData, returns the CIP reply (from the 0xB2 item).
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
    if (!reply || reply->status != 0) {
        std::printf("SendRRData failed (encap status 0x%04X)\n", reply ? reply->status : 0xFFFFu);
        return std::nullopt;
    }
    ByteReader r(reply->data.data(), reply->data.size());
    r.u32();
    r.u16();
    uint16_t count = r.u16();
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t type = r.u16();
        uint16_t len = r.u16();
        const uint8_t* item = r.take(len);
        if (type == cpf::kUnconnectedData)
            return std::vector<uint8_t>(item, item + len);
    }
    return std::nullopt;
}

// Returns general status; fills ext status and reply data.
uint8_t parseCipReply(const std::vector<uint8_t>& reply, uint16_t& ext, std::vector<uint8_t>& data)
{
    ByteReader r(reply.data(), reply.size());
    r.u8();
    r.u8();
    uint8_t status = r.u8();
    uint8_t extWords = r.u8();
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

const std::vector<uint8_t> kConnMgrPath = {0x20, 0x06, 0x24, 0x01};

} // namespace

int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        unsigned long v = std::strtoul(argv[i + 1], nullptr, 0);
        if (k == "--target") o.target = argv[i + 1];
        else if (k == "--rpi-ms") o.rpiMs = uint32_t(v);
        else if (k == "--seconds") o.seconds = int(v);
        else if (k == "--local-port") o.localPort = uint16_t(v);
        else if (k == "--in-size") o.inSize = uint16_t(v);
        else if (k == "--out-size") o.outSize = uint16_t(v);
        else if (k == "--o2t") o.o2tInstance = uint16_t(v);
        else if (k == "--t2o") o.t2oInstance = uint16_t(v);
        else if (k == "--cfg") o.cfgInstance = uint16_t(v);
        else if (k == "--skip-close") o.skipClose = v != 0;
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }

    SocketLibrary lib;
    sockaddr_in target{};
    target.sin_family = AF_INET;
    if (inet_pton(AF_INET, o.target.c_str(), &target.sin_addr) != 1) {
        std::printf("bad target address\n");
        return 2;
    }

    // ---- 1. UDP ListIdentity --------------------------------------------
    {
        socket_t u = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        setRecvTimeoutMs(u, 1000);
        sockaddr_in to = target;
        to.sin_port = htons(kEncapPort);
        auto pkt = encapPacket(encap::kListIdentity, 0, {});
        sendTo(u, pkt.data(), pkt.size(), to);
        uint8_t buf[600];
        sockaddr_in from{};
        int n = recvFrom(u, buf, sizeof buf, from);
        closeSocket(u);
        if (n < 24 + 2 + 4 + 2 + 16 + 14 + 1) {
            std::printf("ListIdentity: no reply\n");
            return 1;
        }
        ByteReader r(buf + 24, size_t(n) - 24);
        r.u16(); // count
        r.u16(); // type
        r.u16(); // len
        r.u16(); // version
        r.skip(16);
        uint16_t vendor = r.u16();
        uint16_t devType = r.u16();
        uint16_t product = r.u16();
        uint8_t maj = r.u8(), min = r.u8();
        r.u16();
        uint32_t serial = r.u32();
        uint8_t nameLen = r.u8();
        std::string name(reinterpret_cast<const char*>(r.take(nameLen)), nameLen);
        std::printf("ListIdentity: \"%s\" vendor %u type %u product %u rev %u.%u serial %08X\n", name.c_str(),
                    vendor, devType, product, maj, min, serial);
    }

    // ---- 2. TCP session ---------------------------------------------------
    socket_t tcp = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in to = target;
    to.sin_port = htons(kEncapPort);
    if (::connect(tcp, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0) {
        std::printf("TCP connect failed\n");
        return 1;
    }
    setRecvTimeoutMs(tcp, 2000);
    auto reg = transact(tcp, encap::kRegisterSession, 0, {1, 0, 0, 0});
    if (!reg || reg->status != 0) {
        std::printf("RegisterSession failed\n");
        return 1;
    }
    uint32_t session = reg->session;
    std::printf("RegisterSession: handle 0x%08X\n", session);

    // ---- 3. Read Identity product name -----------------------------------
    {
        auto reply = cipRequest(tcp, session, {cip::kGetAttributeSingle, 3, 0x20, 0x01, 0x24, 0x01, 0x30, 0x07});
        uint16_t ext;
        std::vector<uint8_t> data;
        if (!reply || parseCipReply(*reply, ext, data) != 0 || data.empty()) {
            std::printf("Get_Attribute_Single(Identity.7) failed\n");
            return 1;
        }
        std::printf("Identity.ProductName = \"%.*s\"\n", int(data[0]), reinterpret_cast<const char*>(&data[1]));
    }

    // ---- 4. Forward_Open --------------------------------------------------
    std::mt19937 rng(std::random_device{}());
    uint16_t connSerial = uint16_t(rng());
    const uint16_t origVendor = 0xFFFE;
    const uint32_t origSerial = 0x51A11234;
    uint32_t rpiUs = o.rpiMs * 1000;

    std::vector<uint8_t> connPath = {0x20, 0x04};
    appendLogical(connPath, 0x24, o.cfgInstance);
    appendLogical(connPath, 0x2C, o.o2tInstance);
    appendLogical(connPath, 0x2C, o.t2oInstance);

    ByteWriter fo;
    fo.u8(cip::kForwardOpen);
    fo.u8(2);
    fo.bytes(kConnMgrPath);
    fo.u8(0x0A); // priority / time tick
    fo.u8(0x0E); // timeout ticks
    fo.u32(0);   // O->T ID: chosen by target
    fo.u32(rng()); // T->O ID proposal
    fo.u16(connSerial);
    fo.u16(origVendor);
    fo.u32(origSerial);
    fo.u8(1); // timeout multiplier: x8
    fo.zeros(3);
    fo.u32(rpiUs);
    fo.u16(uint16_t(0x4800 | ((o.outSize + 6) & 0x1FF))); // P2P, scheduled, fixed
    fo.u32(rpiUs);
    fo.u16(uint16_t(0x4800 | ((o.inSize + 2) & 0x1FF)));
    fo.u8(0x01); // class 1, cyclic
    fo.u8(uint8_t(connPath.size() / 2));
    fo.bytes(connPath);

    auto foReply = cipRequest(tcp, session, fo.data(), o.localPort);
    uint16_t ext = 0;
    std::vector<uint8_t> foData;
    if (!foReply) {
        std::printf("Forward_Open: no reply\n");
        return 1;
    }
    uint8_t st = parseCipReply(*foReply, ext, foData);
    if (st != 0) {
        std::printf("Forward_Open REJECTED: general 0x%02X extended 0x%04X\n", st, ext);
        return 3;
    }
    ByteReader fr(foData.data(), foData.size());
    uint32_t o2tId = fr.u32();
    uint32_t t2oId = fr.u32();
    std::printf("Forward_Open OK: O->T id 0x%08X, T->O id 0x%08X, RPI %u ms\n", o2tId, t2oId, o.rpiMs);

    // ---- 5. Cyclic I/O ----------------------------------------------------
    socket_t udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    setReuseAddr(udp);
    disableUdpConnReset(udp);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(o.localPort);
    if (::bind(udp, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        std::printf("cannot bind UDP %u\n", o.localPort);
        return 1;
    }
    sockaddr_in ioTarget = target;
    ioTarget.sin_port = htons(kIoPort);

    std::vector<uint8_t> outputs(o.outSize, 0);
    uint32_t encapSeq = 0;
    uint16_t cipSeq = 0;
    uint32_t received = 0, echoMatches = 0;
    uint32_t lastHeartbeat = 0;
    auto start = Clock::now();
    auto nextSend = start;
    auto end = start + std::chrono::seconds(o.seconds);
    auto nextPrint = start + std::chrono::seconds(1);

    while (Clock::now() < end) {
        auto now = Clock::now();
        if (now >= nextSend) {
            // Output pattern changes every 100 ms so the echo is observable.
            uint32_t tick = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() / 100);
            for (size_t i = 0; i < outputs.size(); ++i)
                outputs[i] = uint8_t(tick + i);
            ByteWriter w;
            w.u16(2);
            w.u16(cpf::kSequencedAddress);
            w.u16(8);
            w.u32(o2tId);
            w.u32(++encapSeq);
            w.u16(cpf::kConnectedData);
            w.u16(uint16_t(2 + 4 + outputs.size()));
            w.u16(++cipSeq);
            w.u32(1); // run/idle header: RUN
            w.bytes(outputs);
            sendTo(udp, w.data().data(), w.size(), ioTarget);
            nextSend += std::chrono::milliseconds(o.rpiMs);
        }

        auto wait = std::chrono::duration_cast<std::chrono::microseconds>(nextSend - Clock::now());
        if (wait.count() < 0)
            wait = std::chrono::microseconds(0);
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(udp, &rs);
        timeval tv{0, static_cast<long>(wait.count())};
        if (::select(static_cast<int>(udp + 1), &rs, nullptr, nullptr, &tv) > 0) {
            uint8_t buf[1500];
            sockaddr_in from{};
            int n = recvFrom(udp, buf, sizeof buf, from);
            if (n >= 2 + 4 + 8 + 4 + 2) {
                ByteReader r(buf, size_t(n));
                r.u16();
                r.u16();
                r.u16();
                uint32_t id = r.u32();
                r.u32();
                r.u16();
                uint16_t len = r.u16();
                if (id == t2oId && len == o.inSize + 2 && r.remaining() >= len) {
                    r.u16(); // sequence count
                    const uint8_t* in = r.take(o.inSize);
                    ++received;
                    if (o.inSize >= 4)
                        lastHeartbeat = uint32_t(in[0] | (in[1] << 8) | (in[2] << 16) | (uint32_t(in[3]) << 24));
                    if (o.inSize > 4 && o.outSize >= o.inSize &&
                        std::memcmp(in + 4, outputs.data() + 4, o.inSize - 4u) == 0)
                        ++echoMatches;
                }
            }
        }

        if (Clock::now() >= nextPrint) {
            std::printf("  T->O packets %u, echo matches %u, adapter heartbeat %u\n", received, echoMatches,
                        lastHeartbeat);
            nextPrint += std::chrono::seconds(1);
        }
    }

    // ---- 6. Forward_Close / teardown -------------------------------------
    bool closeOk = true;
    if (!o.skipClose) {
        ByteWriter fc;
        fc.u8(cip::kForwardClose);
        fc.u8(2);
        fc.bytes(kConnMgrPath);
        fc.u8(0x0A);
        fc.u8(0x0E);
        fc.u16(connSerial);
        fc.u16(origVendor);
        fc.u32(origSerial);
        fc.u8(uint8_t(connPath.size() / 2));
        fc.u8(0);
        fc.bytes(connPath);
        auto fcReply = cipRequest(tcp, session, fc.data());
        std::vector<uint8_t> d;
        closeOk = fcReply && parseCipReply(*fcReply, ext, d) == 0;
        std::printf("Forward_Close: %s\n", closeOk ? "OK" : "FAILED");
        auto unreg = encapPacket(encap::kUnRegisterSession, session, {});
        sendAll(tcp, unreg.data(), unreg.size());
    } else {
        std::printf("Forward_Close skipped: adapter watchdog should time the connection out\n");
    }
    closeSocket(tcp);
    closeSocket(udp);

    uint32_t expected = uint32_t(o.seconds * 1000 / o.rpiMs);
    bool pass = closeOk && received > expected / 2 && echoMatches > 0;
    std::printf("RESULT: %s (T->O %u of ~%u expected, echo matches %u)\n", pass ? "PASS" : "FAIL", received, expected,
                echoMatches);
    return pass ? 0 : 1;
}
