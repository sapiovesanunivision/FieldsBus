// mb_client_test: automated test of the Modbus CLIENT (softmb::ModbusClient) against the Modbus
// SERVER demo (mb_server_demo). It exercises every supported function code through the client API,
// the server's exception paths (raw requests the client would refuse), the unit-id filter, TCP
// pipelining, client-side validation / timeouts / reconnect, and measures the request rate.
//
// Usage: mb_client_test [--target IP] [--port N] [--transport tcp|udp] [--unit N]
//                       [--in-size N] [--out-size N] [--seconds N]
// Sizes must match the server (defaults match mb_server_demo: 64 / 64, unit 1).
#include "softmb/modbus_client.hpp"
#include "softmb/modbus_defs.hpp"

#include "softeip/bytes.hpp"
#include "softeip/socket_compat.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace softmb;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string target = "127.0.0.1";
    uint16_t port = 502;
    bool udp = false;
    uint8_t unit = 1;
    size_t inSize = 64;  // server input area  = holding registers / coils
    size_t outSize = 64; // server output area = input registers / discrete inputs
    int seconds = 2;
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

std::vector<uint8_t> readRequest(uint8_t function, size_t start, size_t qty)
{
    std::vector<uint8_t> v{function};
    be16(v, start);
    be16(v, qty);
    return v;
}

// Raw request the client API would refuse locally: the server must answer with `code`.
void expectRawException(ModbusClient& c, const std::vector<uint8_t>& req, uint8_t code, const std::string& what)
{
    std::vector<uint8_t> resp;
    Result r = c.transact(req, resp);
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s -> exception %02X", what.c_str(), code);
    check(r.code == ResultCode::Exception && r.exception == code, std::string(buf) + " [" + r.text() + "]");
}

void expectException(const Result& r, uint8_t code, const std::string& what)
{
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s -> exception %02X", what.c_str(), code);
    check(r.code == ResultCode::Exception && r.exception == code, std::string(buf) + " [" + r.text() + "]");
}

std::vector<uint16_t> toRegs(const std::vector<uint8_t>& bytes)
{
    std::vector<uint16_t> regs(bytes.size() / 2);
    for (size_t i = 0; i < regs.size(); ++i)
        regs[i] = uint16_t((bytes[i * 2] << 8) | bytes[i * 2 + 1]);
    return regs;
}

// Three requests in ONE TCP segment (raw socket: the client API is one-request-at-a-time).
bool pipelineTest(const Options& o, const std::vector<uint16_t>& expect)
{
    using namespace softeip;
    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(o.port);
    inet_pton(AF_INET, o.target.c_str(), &a.sin_addr);
    bool ok = s != kInvalidSocket && connectWithTimeout(s, a, 1000);
    std::vector<uint8_t> burst;
    for (int i = 0; ok && i < 3; ++i) {
        ByteWriter w;
        w.u16be(uint16_t(100 + i));
        w.u16be(0);
        w.u16be(6);
        w.u8(o.unit);
        w.bytes(readRequest(fc::kReadHoldingRegisters, size_t(i), 1));
        burst.insert(burst.end(), w.data().begin(), w.data().end());
    }
    ok = ok && sendAll(s, burst.data(), burst.size());
    setRecvTimeoutMs(s, 1000);
    for (int i = 0; ok && i < 3; ++i) {
        uint8_t buf[11];
        size_t got = 0;
        while (ok && got < sizeof buf) {
            int r = recvBytes(s, buf + got, sizeof buf - got);
            ok = r > 0;
            if (ok)
                got += size_t(r);
        }
        ok = ok && ((buf[0] << 8) | buf[1]) == 100 + i && buf[7] == 0x03 &&
             uint16_t((buf[9] << 8) | buf[10]) == expect[size_t(i)];
    }
    if (s != kInvalidSocket)
        closeSocket(s);
    return ok;
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

    ModbusClientConfig cfg;
    cfg.host = o.target;
    cfg.port = o.port;
    cfg.transport = o.udp ? ClientTransport::Udp : ClientTransport::Tcp;
    cfg.unitId = o.unit;
    cfg.onLog = [](const std::string& m) { std::printf("  (client) %s\n", m.c_str()); };
    ModbusClient c(cfg);
    std::string error;
    if (!c.connect(&error)) {
        std::printf("cannot open %s connection to %s:%u (%s)\n", o.udp ? "UDP" : "TCP", o.target.c_str(), o.port,
                    error.c_str());
        return 1;
    }
    std::printf("Modbus %s client -> server %s:%u unit %u\n", o.udp ? "UDP" : "TCP", o.target.c_str(), o.port, o.unit);
    const uint16_t holdingRegs = uint16_t(o.inSize / 2);
    const uint16_t inputRegs = uint16_t(o.outSize / 2);

    // ---- FC43/14 device identification (raw PDU) ---------------------
    {
        std::vector<uint8_t> r;
        bool ok = c.transact(pdu({0x2B, 0x0E, 0x01, 0x00}), r).ok() && r.size() > 7 && r[6] == 3;
        std::string ident;
        if (ok) {
            size_t p = 7;
            for (int i = 0; i < 3 && p + 2 <= r.size(); ++i) {
                size_t len = r[p + 1];
                ident += (i ? " / " : "") + std::string(reinterpret_cast<const char*>(&r[p + 2]), len);
                p += 2 + len;
            }
        }
        check(ok, "FC43/14 device identification: " + ident);
    }

    // ---- FC16 write multiple registers + FC03 read back --------------
    std::vector<uint8_t> patternBytes(size_t(holdingRegs) * 2);
    for (size_t i = 0; i < patternBytes.size(); ++i)
        patternBytes[i] = uint8_t(0xA0 + i);
    const std::vector<uint16_t> pattern = toRegs(patternBytes);
    {
        Result w = c.writeMultipleRegisters(0, pattern);
        check(w.ok(), "FC16 write " + std::to_string(holdingRegs) + " holding registers [" + w.text() + "]");
        std::vector<uint16_t> d;
        check(c.readHoldingRegisters(0, holdingRegs, d).ok() && d == pattern, "FC03 read back equals written data");
    }

    // ---- FC06 single register ----------------------------------------
    {
        std::vector<uint16_t> d;
        bool ok = c.writeSingleRegister(5, 0xBEEF).ok() && c.readHoldingRegisters(5, 1, d).ok() && d[0] == 0xBEEF;
        check(ok, "FC06 write single register 5 = 0xBEEF");
    }

    // ---- FC22 mask write ---------------------------------------------
    {
        std::vector<uint16_t> d;
        bool ok = c.writeSingleRegister(6, 0x1234).ok() && c.maskWriteRegister(6, 0xF0F0, 0x0505).ok() &&
                  c.readHoldingRegisters(6, 1, d).ok() && d[0] == 0x1535;
        check(ok, "FC22 mask write (0x1234 & F0F0 | 0505 = 0x1535)");
    }

    // ---- FC23 read/write multiple ------------------------------------
    {
        std::vector<uint16_t> d;
        bool ok = c.readWriteMultipleRegisters(0, 12, 10, {0x1122, 0x3344}, d).ok() && d.size() == 12 &&
                  d[10] == 0x1122 && d[11] == 0x3344 && d[5] == 0xBEEF; // register 5 from FC06 still there
        check(ok, "FC23 write regs 10..11, read 0..11 in one request");
    }

    // ---- Coils: FC15 / FC05 / FC01 -----------------------------------
    {
        const uint16_t coilStart = 400;
        const std::vector<bool> coils = {true, false, true, false, false, true, false, true, false, true}; // 0xA5, 0x02
        check(c.writeMultipleCoils(coilStart, coils).ok(), "FC15 write 10 coils at 400");
        bool okSingle = c.writeSingleCoil(450, true).ok();
        std::vector<bool> d, d2;
        bool ok = okSingle && c.readCoils(coilStart, 10, d).ok() && d == coils && c.readCoils(450, 1, d2).ok() && d2[0];
        check(ok, "FC01 read back coils, FC05 single coil 450");
        // Coils share the bytes of the holding registers: coil 400 = byte 50 bit 0 = register 25 high byte.
        std::vector<uint16_t> reg;
        check(c.readHoldingRegisters(25, 1, reg).ok() && (reg[0] >> 8) == 0xA5,
              "coils and holding registers are the same input area");
    }

    // ---- Echo through the application: FC04 / FC02 ------------------
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // demo app cycle is 10 ms
        std::vector<uint16_t> holding, inputs;
        bool okH = c.readHoldingRegisters(0, holdingRegs, holding).ok();
        bool okI = c.readInputRegisters(0, inputRegs, inputs).ok();
        size_t n = std::min<size_t>(holdingRegs, inputRegs);
        bool echo = okH && okI && n > 2 && std::equal(holding.begin() + 2, holding.begin() + std::ptrdiff_t(n),
                                                      inputs.begin() + 2);
        check(echo, "FC04 input registers echo the holding registers (via the app, bytes 4..)");
        std::vector<bool> bits; // discrete inputs 32..47 = output bytes 4..5 = input register 2
        bool okB = c.readDiscreteInputs(32, 16, bits).ok() && okI;
        bool same = okB;
        for (size_t i = 0; okB && i < 16; ++i) {
            const uint8_t byte = i < 8 ? uint8_t(inputs[2] >> 8) : uint8_t(inputs[2]);
            same = same && bits[i] == bool((byte >> (i % 8)) & 1);
        }
        check(same, "FC02 discrete inputs are the bit view of the output area");
        std::vector<uint16_t> hb1, hb2;
        c.readInputRegisters(0, 2, hb1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        c.readInputRegisters(0, 2, hb2);
        check(hb1.size() == 2 && hb2.size() == 2 && hb1 != hb2, "application heartbeat (input registers 0..1) is running");
    }

    // ---- Server exceptions --------------------------------------------
    {
        std::vector<uint16_t> d;
        expectException(c.readHoldingRegisters(holdingRegs, 1, d), ex::kIllegalDataAddress, "FC03 beyond the holding table");
        expectException(c.readInputRegisters(uint16_t(inputRegs - 1), 2, d), ex::kIllegalDataAddress,
                        "FC04 crossing the end of the input registers");
        expectException(c.writeSingleRegister(holdingRegs, 1), ex::kIllegalDataAddress, "FC06 beyond the holding table");
    }
    expectRawException(c, readRequest(fc::kReadHoldingRegisters, 0, 0), ex::kIllegalDataValue, "FC03 quantity 0 (raw)");
    expectRawException(c, readRequest(fc::kReadHoldingRegisters, 0, 126), ex::kIllegalDataValue, "FC03 quantity 126 (raw)");
    expectRawException(c, readRequest(fc::kReadCoils, 0, 2001), ex::kIllegalDataValue, "FC01 quantity 2001 (raw)");
    expectRawException(c, pdu({0x05, 0x00, 0x00, 0x12, 0x34}), ex::kIllegalDataValue, "FC05 value not 0xFF00/0x0000 (raw)");
    expectRawException(c, pdu({0x07}), ex::kIllegalFunction, "unsupported function code 07 (raw)");
    expectRawException(c, pdu({0x10, 0x00, 0x00, 0x00, 0x02, 0x03, 1, 2, 3}), ex::kIllegalDataValue,
                       "FC16 byte count mismatch (raw)");

    // ---- Client-side validation (no bus traffic) ----------------------
    {
        std::vector<uint16_t> d;
        std::vector<bool> b;
        auto t0 = Clock::now();
        bool ok = c.readHoldingRegisters(0, 0, d).code == ResultCode::InvalidArgument &&
                  c.readHoldingRegisters(0, 126, d).code == ResultCode::InvalidArgument &&
                  c.readCoils(0, 2001, b).code == ResultCode::InvalidArgument &&
                  c.readInputRegisters(65535, 2, d).code == ResultCode::InvalidArgument &&
                  c.writeMultipleRegisters(0, std::vector<uint16_t>(124, 0)).code == ResultCode::InvalidArgument &&
                  c.writeMultipleCoils(0, std::vector<bool>(1969, false)).code == ResultCode::InvalidArgument;
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        check(ok && ms < 50, "client rejects out-of-limit requests locally (InvalidArgument, no traffic)");
    }

    // ---- Unit id filter -----------------------------------------------
    {
        std::vector<uint8_t> r;
        Result res = c.transact(readRequest(fc::kReadHoldingRegisters, 0, 1), r, o.unit == 7 ? 8 : 7);
        check(res.code == ResultCode::Timeout, "request to another unit id gets no reply (client: " + res.text() + ")");
        std::vector<uint16_t> d;
        check(c.readHoldingRegisters(0, 1, d).ok(), "client recovers after a timeout (next request answered)");
    }

    // ---- TCP pipelining (raw) and reconnect ---------------------------
    if (!o.udp) {
        check(pipelineTest(o, pattern), "3 requests pipelined in one TCP segment, 3 ordered replies");
        c.close();
        std::vector<uint16_t> d;
        check(c.readHoldingRegisters(0, 1, d).ok() && c.connected(), "client reconnects on demand after close()");
    }

    // ---- No server: timeout (UDP) / connection refused (TCP) ----------
    {
        ModbusClientConfig dead = cfg;
        dead.port = uint16_t(o.port + 1); // nothing listens there
        dead.responseTimeoutMs = 200;
        dead.connectTimeoutMs = 500;
        dead.retries = 1;
        dead.onLog = nullptr;
        ModbusClient none(dead);
        std::vector<uint16_t> d;
        auto t0 = Clock::now();
        Result res = none.readHoldingRegisters(0, 1, d);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        const ResultCode want = o.udp ? ResultCode::Timeout : ResultCode::NotConnected;
        char buf[160];
        std::snprintf(buf, sizeof buf, "no server on port %u -> %s after %.0f ms", unsigned(dead.port), res.text().c_str(), ms);
        check(res.code == want && !none.connected() && ms < 1500, buf);
    }

    // ---- Request rate -------------------------------------------------
    {
        uint32_t count = 0, errors = 0;
        std::vector<uint16_t> d;
        auto end = Clock::now() + std::chrono::seconds(o.seconds);
        auto start = Clock::now();
        while (Clock::now() < end) {
            if (c.readHoldingRegisters(0, holdingRegs, d).ok())
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
