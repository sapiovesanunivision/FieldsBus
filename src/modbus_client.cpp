// Modbus CLIENT (formerly "master"): synchronous requests over Modbus TCP / Modbus UDP.
// The PC sends the requests; the PLC / device is the Modbus server and holds the registers.
#include "softmb/modbus_client.hpp"
#include "softmb/modbus_defs.hpp"

#include "softeip/socket_compat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifndef _WIN32
#include <netdb.h> // getaddrinfo
#endif

namespace softmb {

using softeip::kInvalidSocket;
using softeip::socket_t;

namespace {

using Clock = std::chrono::steady_clock;

unsigned msUntil(Clock::time_point deadline)
{
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? unsigned(left) : 0u;
}

void putU16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

uint16_t getU16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

std::vector<uint8_t> readRequest(uint8_t function, uint16_t address, uint16_t count)
{
    std::vector<uint8_t> v{function};
    putU16(v, address);
    putU16(v, count);
    return v;
}

bool rangeOk(uint16_t address, size_t count) { return size_t(address) + count <= 0x10000; }

} // namespace

const char* resultCodeName(ResultCode code)
{
    switch (code) {
    case ResultCode::Ok: return "ok";
    case ResultCode::Exception: return "exception";
    case ResultCode::Timeout: return "timeout";
    case ResultCode::NotConnected: return "not connected";
    case ResultCode::ProtocolError: return "protocol error";
    case ResultCode::InvalidArgument: return "invalid argument";
    }
    return "?";
}

std::string Result::text() const
{
    if (code != ResultCode::Exception)
        return resultCodeName(code);
    char buf[64];
    std::snprintf(buf, sizeof buf, "exception %02X (%s)", exception, exceptionName(exception));
    return buf;
}

struct ModbusClient::Impl {
    explicit Impl(ModbusClientConfig c) : cfg(std::move(c)) {}
    ~Impl() { closeSocket(); }

    // All below: caller holds `mutex`.
    bool resolve(std::string* error);
    bool open(std::string* error);
    void closeSocket();
    void dropConnection(const std::string& why);
    Result exchange(uint8_t unit, const std::vector<uint8_t>& pdu, std::vector<uint8_t>& response);
    enum class Rx { Ok, Timeout, Broken, Aborted };
    Rx receive(uint16_t tid, Clock::time_point deadline, std::vector<uint8_t>& pdu);
    Rx fillRx(Clock::time_point deadline); // TCP: append whatever arrived to rxBuf
    Rx waitData(Clock::time_point deadline); // Ok = readable; waits in slices so abort() is seen

    void log(const std::string& msg) const
    {
        if (cfg.onLog)
            cfg.onLog(msg);
    }
    std::string peer() const { return cfg.host + ":" + std::to_string(cfg.port); }
    bool isTcp() const { return cfg.transport == ClientTransport::Tcp; }

    ModbusClientConfig cfg;
    softeip::SocketLibrary socketLib;
    mutable std::mutex mutex;

    sockaddr_in server{};
    bool resolved = false;
    socket_t sock = kInvalidSocket;
    uint16_t tid = 0;
    std::vector<uint8_t> rxBuf; // TCP receive buffer (whole frames are parsed from it)
    bool lastConnectFailed = false;
    Clock::time_point lastConnectAttempt{};
    std::atomic<bool> up{false};
    std::atomic<bool> aborting{false}; // set by abort() without the mutex
};

bool ModbusClient::Impl::resolve(std::string* error)
{
    if (resolved)
        return true;
    server = {};
    server.sin_family = AF_INET;
    server.sin_port = htons(cfg.port);
    if (inet_pton(AF_INET, cfg.host.c_str(), &server.sin_addr) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (getaddrinfo(cfg.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            if (error)
                *error = "cannot resolve host " + cfg.host;
            return false;
        }
        server.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    resolved = true;
    return true;
}

bool ModbusClient::Impl::open(std::string* error)
{
    if (sock != kInvalidSocket)
        return true;
    if (!resolve(error))
        return false;
    // TCP: don't hammer an unreachable server; wait reconnectDelayMs after a failed attempt.
    if (isTcp() && lastConnectFailed && Clock::now() - lastConnectAttempt < std::chrono::milliseconds(cfg.reconnectDelayMs)) {
        if (error)
            *error = "reconnect to " + peer() + " delayed after a failed attempt";
        return false;
    }
    lastConnectAttempt = Clock::now();

    sock = ::socket(AF_INET, isTcp() ? SOCK_STREAM : SOCK_DGRAM, isTcp() ? IPPROTO_TCP : IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        if (error)
            *error = "socket() failed";
        return false;
    }
    if (!cfg.bindAddress.empty()) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        if (inet_pton(AF_INET, cfg.bindAddress.c_str(), &local.sin_addr) != 1 ||
            ::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
            closeSocket();
            if (error)
                *error = "cannot bind local address " + cfg.bindAddress;
            return false;
        }
    }
    if (!isTcp()) {
        softeip::disableUdpConnReset(sock);
        return true;
    }
    softeip::setNoDelay(sock);
    if (!softeip::connectWithTimeout(sock, server, cfg.connectTimeoutMs)) {
        closeSocket();
        if (!lastConnectFailed)
            log("cannot connect to Modbus server " + peer());
        lastConnectFailed = true;
        if (error)
            *error = "cannot connect to " + peer();
        return false;
    }
    lastConnectFailed = false;
    up = true;
    log("connected to Modbus server " + peer() + " (TCP)");
    return true;
}

void ModbusClient::Impl::closeSocket()
{
    rxBuf.clear();
    if (sock != kInvalidSocket) {
        softeip::closeSocket(sock);
        sock = kInvalidSocket;
    }
}

void ModbusClient::Impl::dropConnection(const std::string& why)
{
    if (isTcp() && sock != kInvalidSocket)
        log("connection to " + peer() + " closed: " + why);
    if (isTcp())
        closeSocket(); // a TCP stream can't be resynchronized after a lost reply
    up = false;
}

ModbusClient::Impl::Rx ModbusClient::Impl::waitData(Clock::time_point deadline)
{
    // Timeout vs broken is decided here, not by comparing clocks afterwards (msUntil rounds down,
    // so a select with 0 ms can return just before the deadline).
    constexpr unsigned kSliceMs = 50;
    for (;;) {
        const unsigned left = msUntil(deadline);
        const int w = softeip::waitReadable(sock, std::min(left, kSliceMs));
        if (w > 0)
            return Rx::Ok;
        if (w < 0)
            return Rx::Broken;
        if (aborting)
            return Rx::Aborted;
        if (left <= kSliceMs)
            return Rx::Timeout;
    }
}

ModbusClient::Impl::Rx ModbusClient::Impl::fillRx(Clock::time_point deadline)
{
    const Rx w = waitData(deadline);
    if (w != Rx::Ok)
        return w;
    uint8_t chunk[512];
    int r = softeip::recvBytes(sock, chunk, sizeof chunk);
    if (r <= 0)
        return Rx::Broken; // peer closed the connection or socket error
    rxBuf.insert(rxBuf.end(), chunk, chunk + r);
    return Rx::Ok;
}

ModbusClient::Impl::Rx ModbusClient::Impl::receive(uint16_t expectTid, Clock::time_point deadline,
                                                   std::vector<uint8_t>& pdu)
{
    uint8_t buf[kMaxAdu + 16];
    for (;;) {
        size_t n = 0;
        if (isTcp()) {
            // One recv usually brings the whole reply; frames are cut out of rxBuf.
            while (rxBuf.size() < kMbapSize ||
                   rxBuf.size() < 6 + ((size_t(rxBuf[4]) << 8) | rxBuf[5])) {
                if (rxBuf.size() >= kMbapSize) {
                    size_t len = (size_t(rxBuf[4]) << 8) | rxBuf[5]; // unit + PDU
                    if (len < 2 || len > kMaxPdu + 1)
                        return Rx::Broken;
                }
                Rx f = fillRx(deadline);
                if (f != Rx::Ok)
                    return f;
            }
            size_t len = (size_t(rxBuf[4]) << 8) | rxBuf[5];
            if (len < 2 || len > kMaxPdu + 1)
                return Rx::Broken;
            n = 6 + len;
            std::memcpy(buf, rxBuf.data(), n);
            rxBuf.erase(rxBuf.begin(), rxBuf.begin() + std::ptrdiff_t(n));
        } else {
            const Rx w = waitData(deadline);
            if (w != Rx::Ok)
                return w;
            sockaddr_in from{};
            int r = softeip::recvFrom(sock, buf, sizeof buf, from);
            if (r < int(kMbapSize + 1))
                continue;
            if (from.sin_addr.s_addr != server.sin_addr.s_addr || from.sin_port != server.sin_port)
                continue; // not from our server
            n = size_t(r);
            size_t len = (size_t(buf[4]) << 8) | buf[5];
            if (6 + len != n)
                continue; // malformed datagram
        }
        const uint16_t gotTid = getU16(buf);
        const uint16_t protocol = getU16(buf + 2);
        // The transaction id (plus, for UDP, the source address) identifies the reply. The unit id
        // is not compared: some TCP devices and gateways answer with 0 / 0xFF instead of echoing it.
        if (gotTid != expectTid || protocol != 0)
            continue; // stale reply of an earlier (timed-out) request, or not ours: keep waiting
        pdu.assign(buf + kMbapSize, buf + n);
        return Rx::Ok;
    }
}

Result ModbusClient::Impl::exchange(uint8_t unit, const std::vector<uint8_t>& request, std::vector<uint8_t>& response)
{
    if (request.empty() || request.size() > kMaxPdu)
        return {ResultCode::InvalidArgument, 0};

    ResultCode last = ResultCode::Timeout;
    for (unsigned attempt = 0; attempt <= cfg.retries; ++attempt) {
        if (aborting) {
            last = ResultCode::NotConnected;
            break;
        }
        std::string err;
        if (!open(&err)) {
            last = ResultCode::NotConnected; // retries are for lost replies, not for an unreachable server
            break;
        }
        const uint16_t t = ++tid;
        std::vector<uint8_t> adu;
        adu.reserve(kMbapSize + request.size());
        putU16(adu, t);
        putU16(adu, 0);
        putU16(adu, uint16_t(request.size() + 1));
        adu.push_back(unit);
        adu.insert(adu.end(), request.begin(), request.end());

        const bool sent = isTcp() ? softeip::sendAll(sock, adu.data(), adu.size())
                                  : softeip::sendTo(sock, adu.data(), adu.size(), server) == int(adu.size());
        if (!sent) {
            dropConnection("send failed");
            last = ResultCode::NotConnected;
            continue;
        }

        Rx rx = receive(t, Clock::now() + std::chrono::milliseconds(cfg.responseTimeoutMs), response);
        if (rx == Rx::Ok) {
            if (!up && !isTcp())
                log("Modbus server " + peer() + " answering (UDP)");
            up = true;
            if (response.size() >= 2 && response[0] == uint8_t(request[0] | 0x80))
                return {ResultCode::Exception, response[1]};
            if (response.empty() || response[0] != request[0])
                return {ResultCode::ProtocolError, 0};
            return {ResultCode::Ok, 0};
        }
        if (rx == Rx::Aborted) {
            dropConnection("aborted");
            last = ResultCode::NotConnected;
            break;
        }
        if (rx == Rx::Broken) {
            dropConnection("connection broken");
            last = ResultCode::NotConnected;
        } else {
            if (up && !isTcp())
                log("no response from Modbus server " + peer() + " (UDP)");
            dropConnection("response timeout");
            last = ResultCode::Timeout;
        }
    }
    up = false;
    return {last, 0};
}

// ===========================================================================
// Public API
// ===========================================================================

ModbusClient::ModbusClient(ModbusClientConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
ModbusClient::~ModbusClient() = default;

bool ModbusClient::connect(std::string* error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->lastConnectFailed = false; // explicit connect: no reconnect delay
    impl_->aborting = false;
    return impl_->open(error);
}

void ModbusClient::close()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closeSocket();
    impl_->up = false;
    impl_->aborting = false;
}

void ModbusClient::abort() { impl_->aborting = true; }

bool ModbusClient::connected() const { return impl_->up; }
const ModbusClientConfig& ModbusClient::config() const { return impl_->cfg; }

Result ModbusClient::transact(const std::vector<uint8_t>& request, std::vector<uint8_t>& response, int unitOverride)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    uint8_t unit = unitOverride >= 0 ? uint8_t(unitOverride) : impl_->cfg.unitId;
    return impl_->exchange(unit, request, response);
}

namespace {

// Reads `count` bits (FC01/FC02) and validates the byte count.
Result readBitsImpl(ModbusClient& c, uint8_t function, uint16_t address, uint16_t count, std::vector<bool>& values)
{
    if (count < 1 || count > limits::kReadBits || !rangeOk(address, count))
        return {ResultCode::InvalidArgument, 0};
    std::vector<uint8_t> resp;
    Result r = c.transact(readRequest(function, address, count), resp);
    if (!r.ok())
        return r;
    const size_t bytes = (size_t(count) + 7) / 8;
    if (resp.size() != 2 + bytes || resp[1] != bytes)
        return {ResultCode::ProtocolError, 0};
    values = unpackBits(resp.data() + 2, count);
    return r;
}

Result readRegsImpl(ModbusClient& c, uint8_t function, uint16_t address, uint16_t count, std::vector<uint16_t>& values)
{
    if (count < 1 || count > limits::kReadRegisters || !rangeOk(address, count))
        return {ResultCode::InvalidArgument, 0};
    std::vector<uint8_t> resp;
    Result r = c.transact(readRequest(function, address, count), resp);
    if (!r.ok())
        return r;
    if (resp.size() != 2 + size_t(count) * 2 || resp[1] != count * 2)
        return {ResultCode::ProtocolError, 0};
    values.resize(count);
    for (size_t i = 0; i < count; ++i)
        values[i] = getU16(resp.data() + 2 + i * 2);
    return r;
}

// FC05/06/15/16 answer with the first 4 data bytes of the request (address + value/quantity).
Result checkEcho(const Result& r, const std::vector<uint8_t>& req, const std::vector<uint8_t>& resp)
{
    if (!r.ok())
        return r;
    if (resp.size() != 5 || std::memcmp(resp.data(), req.data(), 5) != 0)
        return {ResultCode::ProtocolError, 0};
    return r;
}

} // namespace

Result ModbusClient::readCoils(uint16_t address, uint16_t count, std::vector<bool>& values)
{
    return readBitsImpl(*this, fc::kReadCoils, address, count, values);
}

Result ModbusClient::readDiscreteInputs(uint16_t address, uint16_t count, std::vector<bool>& values)
{
    return readBitsImpl(*this, fc::kReadDiscreteInputs, address, count, values);
}

Result ModbusClient::readHoldingRegisters(uint16_t address, uint16_t count, std::vector<uint16_t>& values)
{
    return readRegsImpl(*this, fc::kReadHoldingRegisters, address, count, values);
}

Result ModbusClient::readInputRegisters(uint16_t address, uint16_t count, std::vector<uint16_t>& values)
{
    return readRegsImpl(*this, fc::kReadInputRegisters, address, count, values);
}

Result ModbusClient::writeSingleCoil(uint16_t address, bool value)
{
    std::vector<uint8_t> req{fc::kWriteSingleCoil};
    putU16(req, address);
    putU16(req, value ? 0xFF00 : 0x0000);
    std::vector<uint8_t> resp;
    return checkEcho(transact(req, resp), req, resp);
}

Result ModbusClient::writeSingleRegister(uint16_t address, uint16_t value)
{
    std::vector<uint8_t> req{fc::kWriteSingleRegister};
    putU16(req, address);
    putU16(req, value);
    std::vector<uint8_t> resp;
    return checkEcho(transact(req, resp), req, resp);
}

Result ModbusClient::writeMultipleCoils(uint16_t address, const std::vector<bool>& values)
{
    if (values.empty() || values.size() > limits::kWriteBits || !rangeOk(address, values.size()))
        return {ResultCode::InvalidArgument, 0};
    std::vector<uint8_t> packed = packBits(values);
    std::vector<uint8_t> req{fc::kWriteMultipleCoils};
    putU16(req, address);
    putU16(req, uint16_t(values.size()));
    req.push_back(uint8_t(packed.size()));
    req.insert(req.end(), packed.begin(), packed.end());
    std::vector<uint8_t> resp;
    return checkEcho(transact(req, resp), req, resp);
}

Result ModbusClient::writeMultipleRegisters(uint16_t address, const std::vector<uint16_t>& values)
{
    if (values.empty() || values.size() > limits::kWriteRegisters || !rangeOk(address, values.size()))
        return {ResultCode::InvalidArgument, 0};
    std::vector<uint8_t> req{fc::kWriteMultipleRegisters};
    putU16(req, address);
    putU16(req, uint16_t(values.size()));
    req.push_back(uint8_t(values.size() * 2));
    for (uint16_t v : values)
        putU16(req, v);
    std::vector<uint8_t> resp;
    return checkEcho(transact(req, resp), req, resp);
}

Result ModbusClient::maskWriteRegister(uint16_t address, uint16_t andMask, uint16_t orMask)
{
    std::vector<uint8_t> req{fc::kMaskWriteRegister};
    putU16(req, address);
    putU16(req, andMask);
    putU16(req, orMask);
    std::vector<uint8_t> resp;
    Result r = transact(req, resp);
    if (r.ok() && resp != req) // FC22 echoes the whole request
        return {ResultCode::ProtocolError, 0};
    return r;
}

Result ModbusClient::readWriteMultipleRegisters(uint16_t readAddress, uint16_t readCount, uint16_t writeAddress,
                                                const std::vector<uint16_t>& writeValues,
                                                std::vector<uint16_t>& readValues)
{
    if (readCount < 1 || readCount > limits::kReadRegisters || !rangeOk(readAddress, readCount) ||
        writeValues.empty() || writeValues.size() > limits::kReadWriteWriteRegisters ||
        !rangeOk(writeAddress, writeValues.size()))
        return {ResultCode::InvalidArgument, 0};
    std::vector<uint8_t> req{fc::kReadWriteMultipleRegisters};
    putU16(req, readAddress);
    putU16(req, readCount);
    putU16(req, writeAddress);
    putU16(req, uint16_t(writeValues.size()));
    req.push_back(uint8_t(writeValues.size() * 2));
    for (uint16_t v : writeValues)
        putU16(req, v);
    std::vector<uint8_t> resp;
    Result r = transact(req, resp);
    if (!r.ok())
        return r;
    if (resp.size() != 2 + size_t(readCount) * 2 || resp[1] != readCount * 2)
        return {ResultCode::ProtocolError, 0};
    readValues.resize(readCount);
    for (size_t i = 0; i < readCount; ++i)
        readValues[i] = getU16(resp.data() + 2 + i * 2);
    return r;
}

} // namespace softmb
