// Minimal Modbus master used to test mb_server_demo without a PLC.
// Exercises every supported function code, the exception paths, the unit-id
// filter and TCP pipelining, then measures the request rate.
//
// Usage: mb_client_test [--target IP] [--port N] [--transport tcp|udp] [--unit N]
//                      [--in-size N] [--out-size N] [--seconds N]
// Sizes must match the slave (defaults match mb_server_demo: 64 / 64, unit 1).
#include "softeip/bytes.hpp"
#include "softeip/socket_compat.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace softeip;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string target = "127.0.0.1";
    uint16_t port = 502;
    bool udp = false;
    uint8_t unit = 1;
    size_t inSize = 64;  // slave input area  = holding registers
    size_t outSize = 64; // slave output area = input registers
    int seconds = 2;
};

class Master {
public:
    explicit Master(const Options& o) : o_(o)
    {
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(o.port);
        inet_pton(AF_INET, o.target.c_str(), &addr_.sin_addr);
    }
    ~Master()
    {
        if (sock_ != kInvalidSocket)
            closeSocket(sock_);
    }

    bool open()
    {
        sock_ = ::socket(AF_INET, o_.udp ? SOCK_DGRAM : SOCK_STREAM, o_.udp ? IPPROTO_UDP : IPPROTO_TCP);
        if (sock_ == kInvalidSocket)
            return false;
        setRecvTimeoutMs(sock_, 500);
        if (o_.udp) {
            disableUdpConnReset(sock_);
            return true;
        }
        setNoDelay(sock_);
        return ::connect(sock_, reinterpret_cast<sockaddr*>(&addr_), sizeof addr_) == 0;
    }

    std::vector<uint8_t> frame(const std::vector<uint8_t>& pdu, uint8_t unit, uint16_t tid) const
    {
        ByteWriter w;
        w.u16be(tid);
        w.u16be(0);
        w.u16be(uint16_t(pdu.size() + 1));
        w.u8(unit);
        w.bytes(pdu);
        return std::move(w.data());
    }

    bool sendRaw(const std::vector<uint8_t>& adu)
    {
        if (o_.udp)
            return sendTo(sock_, adu.data(), adu.size(), addr_) == int(adu.size());
        return sendAll(sock_, adu.data(), adu.size());
    }

    // Receives one ADU; returns its PDU, checks transaction id.
    std::optional<std::vector<uint8_t>> receive(uint16_t tid)
    {
        uint8_t buf[300];
        size_t n = 0;
        if (o_.udp) {
            sockaddr_in from{};
            int r = recvFrom(sock_, buf, sizeof buf, from);
            if (r < 8)
                return std::nullopt;
            n = size_t(r);
        } else {
            if (!recvExact(buf, 7))
                return std::nullopt;
            size_t len = (size_t(buf[4]) << 8) | buf[5];
            if (len < 2 || len > 254 || !recvExact(buf + 7, len - 1))
                return std::nullopt;
            n = 6 + len;
        }
        uint16_t gotTid = uint16_t((buf[0] << 8) | buf[1]);
        if (gotTid != tid) {
            std::printf("  transaction id mismatch: sent %u got %u\n", tid, gotTid);
            return std::nullopt;
        }
        return std::vector<uint8_t>(buf + 7, buf + n);
    }

    std::optional<std::vector<uint8_t>> request(const std::vector<uint8_t>& pdu, uint8_t unit)
    {
        uint16_t tid = ++tid_;
        if (!sendRaw(frame(pdu, unit, tid)))
            return std::nullopt;
        return receive(tid);
    }
    std::optional<std::vector<uint8_t>> request(const std::vector<uint8_t>& pdu) { return request(pdu, o_.unit); }

    uint16_t nextTid() { return ++tid_; }

private:
    bool recvExact(uint8_t* p, size_t n)
    {
        while (n > 0) {
            int r = recvBytes(sock_, p, n);
            if (r <= 0)
                return false;
            p += r;
            n -= size_t(r);
        }
        return true;
    }

    Options o_;
    sockaddr_in addr_{};
    socket_t sock_ = kInvalidSocket;
    uint16_t tid_ = 0;
};

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

std::vector<uint8_t> pdu(std::initializer_list<int> bytes)
{
    std::vector<uint8_t> v;
    for (int b : bytes)
        v.push_back(uint8_t(b));
    return v;
}

void be16(std::vector<uint8_t>& v, size_t x)
{
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

std::vector<uint8_t> readRequest(uint8_t fc, size_t start, size_t qty)
{
    std::vector<uint8_t> v{fc};
    be16(v, start);
    be16(v, qty);
    return v;
}

// Returns the data bytes of a read response, or nullopt on exception/error.
std::optional<std::vector<uint8_t>> readData(Master& m, uint8_t fc, size_t start, size_t qty)
{
    auto r = m.request(readRequest(fc, start, qty));
    if (!r || r->size() < 2 || (*r)[0] != fc || size_t((*r)[1]) != r->size() - 2)
        return std::nullopt;
    return std::vector<uint8_t>(r->begin() + 2, r->end());
}

// Expects an exception response with the given code.
void expectException(Master& m, const std::vector<uint8_t>& req, uint8_t code, const std::string& what)
{
    auto r = m.request(req);
    bool ok = r && r->size() == 2 && (*r)[0] == uint8_t(req[0] | 0x80) && (*r)[1] == code;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s -> exception %02X", what.c_str(), code);
    check(ok, buf);
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        unsigned long n = std::strtoul(v, nullptr, 0);
        if (k == "--target") o.target = v;
        else if (k == "--port") o.port = uint16_t(n);
        else if (k == "--transport") o.udp = std::string(v) == "udp";
        else if (k == "--unit") o.unit = uint8_t(n);
        else if (k == "--in-size") o.inSize = n;
        else if (k == "--out-size") o.outSize = n;
        else if (k == "--seconds") o.seconds = int(n);
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }

    SocketLibrary lib;
    Master m(o);
    if (!m.open()) {
        std::printf("cannot open %s connection to %s:%u\n", o.udp ? "UDP" : "TCP", o.target.c_str(), o.port);
        return 1;
    }
    std::printf("Modbus %s master -> %s:%u unit %u\n", o.udp ? "UDP" : "TCP", o.target.c_str(), o.port, o.unit);
    const size_t holdingRegs = o.inSize / 2;
    const size_t inputRegs = o.outSize / 2;

    // ---- FC43/14 device identification -------------------------------
    {
        auto r = m.request(pdu({0x2B, 0x0E, 0x01, 0x00}));
        bool ok = r && r->size() > 7 && (*r)[0] == 0x2B && (*r)[6] == 3;
        std::string ident;
        if (ok) {
            size_t p = 7;
            for (int i = 0; i < 3 && p + 2 <= r->size(); ++i) {
                size_t len = (*r)[p + 1];
                ident += (i ? " / " : "") + std::string(reinterpret_cast<const char*>(&(*r)[p + 2]), len);
                p += 2 + len;
            }
        }
        check(ok, "FC43/14 device identification: " + ident);
    }

    // ---- FC16 write multiple registers + FC03 read back --------------
    std::vector<uint8_t> pattern(holdingRegs * 2);
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = uint8_t(0xA0 + i);
    {
        std::vector<uint8_t> req{0x10};
        be16(req, 0);
        be16(req, holdingRegs);
        req.push_back(uint8_t(pattern.size()));
        req.insert(req.end(), pattern.begin(), pattern.end());
        auto r = m.request(req);
        check(r && r->size() == 5 && (*r)[0] == 0x10, "FC16 write " + std::to_string(holdingRegs) + " holding registers");
        auto d = readData(m, 0x03, 0, holdingRegs);
        check(d && *d == pattern, "FC03 read back equals written data");
    }

    // ---- FC06 single register ----------------------------------------
    {
        auto r = m.request(pdu({0x06, 0x00, 0x05, 0xBE, 0xEF}));
        auto d = readData(m, 0x03, 5, 1);
        check(r && *r == pdu({0x06, 0x00, 0x05, 0xBE, 0xEF}) && d && *d == pdu({0xBE, 0xEF}),
              "FC06 write single register 5 = 0xBEEF");
    }

    // ---- FC22 mask write ---------------------------------------------
    {
        m.request(pdu({0x06, 0x00, 0x06, 0x12, 0x34}));
        auto r = m.request(pdu({0x16, 0x00, 0x06, 0xF0, 0xF0, 0x05, 0x05}));
        auto d = readData(m, 0x03, 6, 1);
        check(r && r->size() == 7 && d && *d == pdu({0x15, 0x35}), "FC22 mask write (0x1234 & F0F0 | 0505 = 0x1535)");
    }

    // ---- FC23 read/write multiple ------------------------------------
    {
        std::vector<uint8_t> req{0x17};
        be16(req, 0);  // read start
        be16(req, 12); // read qty
        be16(req, 10); // write start
        be16(req, 2);  // write qty
        req.push_back(4);
        for (int b : {0x11, 0x22, 0x33, 0x44})
            req.push_back(uint8_t(b));
        auto r = m.request(req);
        bool ok = r && r->size() == 2 + 24 && (*r)[0] == 0x17 && (*r)[2 + 20] == 0x11 && (*r)[2 + 23] == 0x44 &&
                  (*r)[2 + 10] == 0xBE; // register 5 from FC06 still there
        check(ok, "FC23 write regs 10..11, read 0..11 in one request");
    }

    // ---- Coils: FC15 / FC05 / FC01 -----------------------------------
    {
        const size_t coilStart = 400;
        std::vector<uint8_t> req{0x0F};
        be16(req, coilStart);
        be16(req, 10);
        req.push_back(2);
        req.push_back(0xA5); // coils 400..407 = 1,0,1,0,0,1,0,1
        req.push_back(0x02); // coil 409 = 1
        auto r = m.request(req);
        check(r && r->size() == 5 && (*r)[0] == 0x0F, "FC15 write 10 coils at 400");
        m.request(pdu({0x05, 0x01, 0xC2, 0xFF, 0x00})); // coil 450 ON
        auto d = readData(m, 0x01, coilStart, 10);
        auto d2 = readData(m, 0x01, 450, 1);
        check(d && *d == pdu({0xA5, 0x02}) && d2 && *d2 == pdu({0x01}), "FC01 read back coils, FC05 single coil 450");
        // Coils share the bytes of the holding registers: coil 400 = byte 50 bit 0 = register 25 high byte.
        auto reg = readData(m, 0x03, 25, 1);
        check(reg && (*reg)[0] == 0xA5, "coils and holding registers are the same input area");
    }

    // ---- Echo through the application: FC04 / FC02 ------------------
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // demo app cycle is 10 ms
        auto holding = readData(m, 0x03, 0, holdingRegs);
        auto inputs = readData(m, 0x04, 0, inputRegs);
        size_t n = std::min(holdingRegs, inputRegs) * 2;
        bool echo = holding && inputs && n > 4 && std::memcmp(holding->data() + 4, inputs->data() + 4, n - 4) == 0;
        check(echo, "FC04 input registers echo the holding registers (via the app, bytes 4..)");
        auto bits = readData(m, 0x02, 32, 16); // discrete inputs 32..47 = output bytes 4..5
        check(bits && inputs && (*bits)[0] == (*inputs)[4] && (*bits)[1] == (*inputs)[5],
              "FC02 discrete inputs are the bit view of the output area");
        auto hb1 = readData(m, 0x04, 0, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto hb2 = readData(m, 0x04, 0, 2);
        check(hb1 && hb2 && *hb1 != *hb2, "application heartbeat (input registers 0..1) is running");
    }

    // ---- Exceptions ---------------------------------------------------
    expectException(m, readRequest(0x03, holdingRegs, 1), 0x02, "FC03 beyond the holding table");
    expectException(m, readRequest(0x04, inputRegs - 1, 2), 0x02, "FC04 crossing the end of the input registers");
    expectException(m, readRequest(0x03, 0, 0), 0x03, "FC03 quantity 0");
    expectException(m, readRequest(0x03, 0, 126), 0x03, "FC03 quantity 126");
    expectException(m, readRequest(0x01, 0, 2001), 0x03, "FC01 quantity 2001");
    expectException(m, pdu({0x05, 0x00, 0x00, 0x12, 0x34}), 0x03, "FC05 value not 0xFF00/0x0000");
    {
        std::vector<uint8_t> req{0x06};
        be16(req, holdingRegs);
        be16(req, 1);
        expectException(m, req, 0x02, "FC06 beyond the holding table");
    }
    expectException(m, pdu({0x07}), 0x01, "unsupported function code 07");
    expectException(m, pdu({0x10, 0x00, 0x00, 0x00, 0x02, 0x03, 1, 2, 3}), 0x03, "FC16 byte count mismatch");

    // ---- Unit id filter -----------------------------------------------
    {
        auto r = m.request(readRequest(0x03, 0, 1), uint8_t(o.unit == 7 ? 8 : 7));
        check(!r, "request to another unit id gets no reply");
    }

    // ---- TCP pipelining -----------------------------------------------
    if (!o.udp) {
        std::vector<uint8_t> burst;
        uint16_t tids[3];
        for (int i = 0; i < 3; ++i) {
            tids[i] = m.nextTid();
            auto f = m.frame(readRequest(0x03, size_t(i), 1), o.unit, tids[i]);
            burst.insert(burst.end(), f.begin(), f.end());
        }
        m.sendRaw(burst);
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            auto r = m.receive(tids[i]);
            ok = ok && r && r->size() == 4 && (*r)[2] == pattern[size_t(i) * 2] && (*r)[3] == pattern[size_t(i) * 2 + 1];
        }
        check(ok, "3 requests pipelined in one TCP segment, 3 ordered replies");
    }

    // ---- Request rate -------------------------------------------------
    {
        uint32_t count = 0, errors = 0;
        auto end = Clock::now() + std::chrono::seconds(o.seconds);
        auto start = Clock::now();
        while (Clock::now() < end) {
            if (readData(m, 0x03, 0, holdingRegs))
                ++count;
            else
                ++errors;
        }
        double secs = std::chrono::duration<double>(Clock::now() - start).count();
        std::printf("  rate: %.0f FC03 requests/s (%u ok, %u errors), avg round trip %.3f ms\n", count / secs, count,
                    errors, count ? secs * 1000.0 / count : 0.0);
        check(errors == 0 && count > 0, "sustained polling without errors");
    }

    std::printf("RESULT: %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
