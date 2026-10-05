// Software Modbus slave: PDU engine + Modbus TCP / Modbus UDP transports.
#include "softmb/modbus_slave.hpp"

#include "softeip/bytes.hpp"
#include "softeip/socket_compat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace softmb {

using softeip::ByteReader;
using softeip::ByteWriter;
using softeip::kInvalidSocket;
using softeip::ParseError;
using softeip::socket_t;

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMbapSize = 7;        // transaction, protocol, length, unit
constexpr size_t kMaxPdu = 253;
constexpr size_t kMaxAdu = kMbapSize + kMaxPdu;

namespace fc {
constexpr uint8_t kReadCoils = 0x01;
constexpr uint8_t kReadDiscreteInputs = 0x02;
constexpr uint8_t kReadHoldingRegisters = 0x03;
constexpr uint8_t kReadInputRegisters = 0x04;
constexpr uint8_t kWriteSingleCoil = 0x05;
constexpr uint8_t kWriteSingleRegister = 0x06;
constexpr uint8_t kWriteMultipleCoils = 0x0F;
constexpr uint8_t kWriteMultipleRegisters = 0x10;
constexpr uint8_t kMaskWriteRegister = 0x16;
constexpr uint8_t kReadWriteMultipleRegisters = 0x17;
constexpr uint8_t kEncapsulatedInterface = 0x2B;
constexpr uint8_t kMeiReadDeviceId = 0x0E;
} // namespace fc

namespace ex {
constexpr uint8_t kIllegalFunction = 0x01;
constexpr uint8_t kIllegalDataAddress = 0x02;
constexpr uint8_t kIllegalDataValue = 0x03;
} // namespace ex

std::vector<uint8_t> exception(uint8_t function, uint8_t code)
{
    return {uint8_t(function | 0x80), code};
}

std::string ipToString(const sockaddr_in& a)
{
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, const_cast<in_addr*>(&a.sin_addr), buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
}

struct TcpClient {
    socket_t sock = kInvalidSocket;
    sockaddr_in peer{};
    std::vector<uint8_t> rx;
    Clock::time_point lastActivity;
};

// One register table entry resolved to a byte area.
struct RegRef {
    std::vector<uint8_t>* area;
    size_t byteOffset;
};

} // namespace

struct ModbusSlave::Impl {
    explicit Impl(ModbusSlaveConfig c)
        : cfg(std::move(c)),
          input((cfg.inputSize + 1) & ~size_t(1), 0),   // rounded up to whole registers
          output((cfg.outputSize + 1) & ~size_t(1), 0)
    {
    }
    ~Impl() { stop(); }

    bool start(std::string* error);
    void stop();
    void run();

    void acceptClient();
    void handleTcpReadable(TcpClient& c, bool& closeClient);
    void handleUdp();
    // Parses one complete MBAP frame; returns the response ADU (empty = no reply).
    std::vector<uint8_t> handleAdu(const uint8_t* adu, size_t n);
    bool unitAccepted(uint8_t unit) const;
    void updateMasterState(Clock::time_point now);

    std::vector<uint8_t> processPdu(const uint8_t* pdu, size_t len);
    std::vector<uint8_t> execute(uint8_t function, ByteReader& r, bool& inputsChanged);
    std::vector<uint8_t> readDeviceId(ByteReader& r);

    // Register / bit address resolution (caller holds dataMutex).
    size_t inputRegisters() const { return input.size() / 2; }
    size_t outputRegisters() const { return output.size() / 2; }
    bool holdingRef(size_t reg, bool forWrite, RegRef& ref);
    static bool getBit(const std::vector<uint8_t>& a, size_t bit) { return (a[bit / 8] >> (bit % 8)) & 1; }
    static void setBit(std::vector<uint8_t>& a, size_t bit, bool v)
    {
        if (v)
            a[bit / 8] = uint8_t(a[bit / 8] | (1u << (bit % 8)));
        else
            a[bit / 8] = uint8_t(a[bit / 8] & ~(1u << (bit % 8)));
    }

    void log(const std::string& msg) const
    {
        if (cfg.onLog)
            cfg.onLog(msg);
    }

    ModbusSlaveConfig cfg;
    softeip::SocketLibrary socketLib;

    std::thread thread;
    std::atomic<bool> running{false};

    socket_t tcpListen = kInvalidSocket;
    socket_t udp = kInvalidSocket;
    std::vector<std::unique_ptr<TcpClient>> clients;
    Clock::time_point lastRequest{};
    bool haveRequest = false;

    mutable std::mutex dataMutex;
    std::vector<uint8_t> input;  // PLC -> PC, guarded by dataMutex
    std::vector<uint8_t> output; // PC -> PLC, guarded by dataMutex
    std::atomic<bool> masterUp{false};
};

// ===========================================================================
// Lifecycle
// ===========================================================================

bool ModbusSlave::Impl::start(std::string* error)
{
    auto fail = [&](const std::string& what) {
        if (error)
            *error = what;
        stop();
        return false;
    };
    if (!cfg.enableTcp && !cfg.enableUdp)
        return fail("neither TCP nor UDP enabled");

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg.port);
    if (inet_pton(AF_INET, cfg.bindAddress.c_str(), &a.sin_addr) != 1)
        return fail("invalid bind address " + cfg.bindAddress);
    const std::string portText = std::to_string(cfg.port);

    if (cfg.enableTcp) {
        tcpListen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcpListen == kInvalidSocket)
            return fail("socket() failed");
        softeip::setReuseAddr(tcpListen);
        if (::bind(tcpListen, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
            return fail("cannot bind TCP " + portText + " (port in use or needs privileges)");
        if (::listen(tcpListen, 8) != 0)
            return fail("listen() failed");
    }
    if (cfg.enableUdp) {
        udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp == kInvalidSocket)
            return fail("socket() failed");
        softeip::setReuseAddr(udp);
        softeip::disableUdpConnReset(udp);
        if (::bind(udp, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
            return fail("cannot bind UDP " + portText + " (port in use or needs privileges)");
    }

    running = true;
    thread = std::thread([this] { run(); });
    log("Modbus slave started on " + cfg.bindAddress + ":" + portText + " (" +
        (cfg.enableTcp ? "TCP" : "") + (cfg.enableTcp && cfg.enableUdp ? "+" : "") + (cfg.enableUdp ? "UDP" : "") +
        "), input " + std::to_string(cfg.inputSize) + " B / output " + std::to_string(cfg.outputSize) + " B");
    return true;
}

void ModbusSlave::Impl::stop()
{
    running = false;
    if (thread.joinable())
        thread.join();
    for (auto& c : clients)
        softeip::closeSocket(c->sock);
    clients.clear();
    for (socket_t* s : {&tcpListen, &udp}) {
        if (*s != kInvalidSocket) {
            softeip::closeSocket(*s);
            *s = kInvalidSocket;
        }
    }
    masterUp = false;
    haveRequest = false;
}

void ModbusSlave::Impl::run()
{
#ifdef _WIN32
    if (cfg.raiseThreadPriority)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    while (running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        socket_t maxFd = 0;
        auto add = [&](socket_t s) {
            if (s == kInvalidSocket)
                return;
            FD_SET(s, &readSet);
            maxFd = std::max(maxFd, s);
        };
        add(tcpListen);
        add(udp);
        for (auto& c : clients)
            add(c->sock);

        timeval tv{0, 50000};
        int ready = ::select(static_cast<int>(maxFd + 1), &readSet, nullptr, nullptr, &tv);
        auto now = Clock::now();

        if (ready > 0) {
            if (tcpListen != kInvalidSocket && FD_ISSET(tcpListen, &readSet))
                acceptClient();
            if (udp != kInvalidSocket && FD_ISSET(udp, &readSet))
                handleUdp();
            for (size_t i = 0; i < clients.size();) {
                TcpClient& c = *clients[i];
                bool closeClient = false;
                if (FD_ISSET(c.sock, &readSet))
                    handleTcpReadable(c, closeClient);
                else if (now - c.lastActivity > std::chrono::milliseconds(cfg.tcpIdleTimeoutMs)) {
                    log("TCP client " + ipToString(c.peer) + " idle timeout");
                    closeClient = true;
                }
                if (closeClient) {
                    softeip::closeSocket(c.sock);
                    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        } else {
            for (size_t i = 0; i < clients.size();) {
                if (now - clients[i]->lastActivity > std::chrono::milliseconds(cfg.tcpIdleTimeoutMs)) {
                    log("TCP client " + ipToString(clients[i]->peer) + " idle timeout");
                    softeip::closeSocket(clients[i]->sock);
                    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        }
        updateMasterState(Clock::now());
    }
}

void ModbusSlave::Impl::updateMasterState(Clock::time_point now)
{
    bool up = !clients.empty() ||
              (haveRequest && now - lastRequest < std::chrono::milliseconds(cfg.masterTimeoutMs));
    if (up == masterUp)
        return;
    masterUp = up;
    log(up ? "master connected" : "master disconnected");
    if (cfg.onConnectionChanged)
        cfg.onConnectionChanged(up);
}

// ===========================================================================
// Transports
// ===========================================================================

bool ModbusSlave::Impl::unitAccepted(uint8_t unit) const
{
    return cfg.unitId == 0 || unit == cfg.unitId || unit == 0xFF || unit == 0;
}

std::vector<uint8_t> ModbusSlave::Impl::handleAdu(const uint8_t* adu, size_t n)
{
    ByteReader r(adu, n);
    uint16_t transaction = r.u16be();
    uint16_t protocol = r.u16be();
    r.u16be(); // length, validated by the caller
    uint8_t unit = r.u8();
    if (protocol != 0 || !unitAccepted(unit))
        return {};

    lastRequest = Clock::now();
    haveRequest = true;
    std::vector<uint8_t> resp = processPdu(r.cur(), r.remaining());

    ByteWriter w;
    w.u16be(transaction);
    w.u16be(0);
    w.u16be(uint16_t(resp.size() + 1));
    w.u8(unit);
    w.bytes(resp);
    return std::move(w.data());
}

void ModbusSlave::Impl::acceptClient()
{
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    socket_t s = ::accept(tcpListen, reinterpret_cast<sockaddr*>(&peer), &len);
    if (s == kInvalidSocket)
        return;
    if (clients.size() >= cfg.maxTcpClients) {
        log("TCP client " + ipToString(peer) + " rejected: too many clients");
        softeip::closeSocket(s);
        return;
    }
    softeip::setNoDelay(s);
    auto c = std::make_unique<TcpClient>();
    c->sock = s;
    c->peer = peer;
    c->lastActivity = Clock::now();
    log("TCP client " + ipToString(peer) + " connected");
    clients.push_back(std::move(c));
}

void ModbusSlave::Impl::handleTcpReadable(TcpClient& c, bool& closeClient)
{
    uint8_t buf[2048];
    int n = softeip::recvBytes(c.sock, buf, sizeof buf);
    if (n <= 0) {
        log("TCP client " + ipToString(c.peer) + " disconnected");
        closeClient = true;
        return;
    }
    c.lastActivity = Clock::now();
    c.rx.insert(c.rx.end(), buf, buf + n);

    // Several requests may arrive in one segment, or one request across several.
    while (c.rx.size() >= kMbapSize) {
        size_t length = (size_t(c.rx[4]) << 8) | c.rx[5]; // unit + PDU
        if (length < 2 || length > kMaxPdu + 1) {
            log("TCP client " + ipToString(c.peer) + " sent an invalid MBAP length, closing");
            closeClient = true;
            return;
        }
        size_t total = 6 + length;
        if (c.rx.size() < total)
            break;
        std::vector<uint8_t> reply = handleAdu(c.rx.data(), total);
        c.rx.erase(c.rx.begin(), c.rx.begin() + static_cast<std::ptrdiff_t>(total));
        if (!reply.empty() && !softeip::sendAll(c.sock, reply.data(), reply.size())) {
            closeClient = true;
            return;
        }
    }
}

void ModbusSlave::Impl::handleUdp()
{
    uint8_t buf[kMaxAdu + 16];
    sockaddr_in peer{};
    int n = softeip::recvFrom(udp, buf, sizeof buf, peer);
    if (n < int(kMbapSize + 1))
        return;
    size_t length = (size_t(buf[4]) << 8) | buf[5];
    if (length < 2 || 6 + length != size_t(n)) // one complete ADU per datagram
        return;
    std::vector<uint8_t> reply = handleAdu(buf, size_t(n));
    if (!reply.empty())
        softeip::sendTo(udp, reply.data(), reply.size(), peer);
}

// ===========================================================================
// PDU engine
// ===========================================================================

std::vector<uint8_t> ModbusSlave::Impl::processPdu(const uint8_t* pdu, size_t len)
{
    if (len == 0)
        return {};
    uint8_t function = pdu[0];
    bool inputsChanged = false;
    std::vector<uint8_t> resp;
    std::vector<uint8_t> snapshot;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        try {
            ByteReader r(pdu + 1, len - 1);
            resp = execute(function, r, inputsChanged);
        } catch (const ParseError&) {
            resp = exception(function, ex::kIllegalDataValue); // truncated request
            inputsChanged = false;
        }
        if (inputsChanged && cfg.onInputsChanged)
            snapshot.assign(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(cfg.inputSize));
    }
    if (inputsChanged && cfg.onInputsChanged)
        cfg.onInputsChanged(snapshot);
    return resp;
}

bool ModbusSlave::Impl::holdingRef(size_t reg, bool forWrite, RegRef& ref)
{
    if (reg < inputRegisters()) {
        ref = {&input, reg * 2};
        return true;
    }
    if (!forWrite && cfg.outputsInHoldingAt >= 0) {
        size_t base = size_t(cfg.outputsInHoldingAt);
        if (reg >= base && reg - base < outputRegisters()) {
            ref = {&output, (reg - base) * 2};
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> ModbusSlave::Impl::execute(uint8_t function, ByteReader& r, bool& inputsChanged)
{
    ByteWriter w;
    w.u8(function);

    switch (function) {
    case fc::kReadCoils:
    case fc::kReadDiscreteInputs: {
        const auto& area = function == fc::kReadCoils ? input : output;
        size_t start = r.u16be();
        size_t qty = r.u16be();
        if (qty < 1 || qty > 2000)
            return exception(function, ex::kIllegalDataValue);
        if (start + qty > area.size() * 8)
            return exception(function, ex::kIllegalDataAddress);
        size_t bytes = (qty + 7) / 8;
        w.u8(uint8_t(bytes));
        std::vector<uint8_t> packed(bytes, 0);
        for (size_t i = 0; i < qty; ++i)
            if (getBit(area, start + i))
                packed[i / 8] = uint8_t(packed[i / 8] | (1u << (i % 8)));
        w.bytes(packed);
        break;
    }

    case fc::kReadHoldingRegisters:
    case fc::kReadInputRegisters: {
        size_t start = r.u16be();
        size_t qty = r.u16be();
        if (qty < 1 || qty > 125)
            return exception(function, ex::kIllegalDataValue);
        w.u8(uint8_t(qty * 2));
        for (size_t i = 0; i < qty; ++i) {
            RegRef ref{};
            if (function == fc::kReadHoldingRegisters) {
                if (!holdingRef(start + i, false, ref))
                    return exception(function, ex::kIllegalDataAddress);
            } else {
                if (start + i >= outputRegisters())
                    return exception(function, ex::kIllegalDataAddress);
                ref = {&output, (start + i) * 2};
            }
            w.u8((*ref.area)[ref.byteOffset]);
            w.u8((*ref.area)[ref.byteOffset + 1]);
        }
        break;
    }

    case fc::kWriteSingleCoil: {
        size_t addr = r.u16be();
        uint16_t value = r.u16be();
        if (value != 0xFF00 && value != 0x0000)
            return exception(function, ex::kIllegalDataValue);
        if (addr >= input.size() * 8)
            return exception(function, ex::kIllegalDataAddress);
        bool v = value == 0xFF00;
        inputsChanged = getBit(input, addr) != v;
        setBit(input, addr, v);
        w.u16be(uint16_t(addr));
        w.u16be(value);
        break;
    }

    case fc::kWriteSingleRegister: {
        size_t addr = r.u16be();
        const uint8_t* v = r.take(2);
        RegRef ref{};
        if (!holdingRef(addr, true, ref))
            return exception(function, ex::kIllegalDataAddress);
        inputsChanged = std::memcmp(&input[ref.byteOffset], v, 2) != 0;
        std::memcpy(&input[ref.byteOffset], v, 2);
        w.u16be(uint16_t(addr));
        w.bytes(v, 2);
        break;
    }

    case fc::kWriteMultipleCoils: {
        size_t start = r.u16be();
        size_t qty = r.u16be();
        size_t byteCount = r.u8();
        if (qty < 1 || qty > 1968 || byteCount != (qty + 7) / 8)
            return exception(function, ex::kIllegalDataValue);
        const uint8_t* bits = r.take(byteCount);
        if (start + qty > input.size() * 8)
            return exception(function, ex::kIllegalDataAddress);
        for (size_t i = 0; i < qty; ++i) {
            bool v = (bits[i / 8] >> (i % 8)) & 1;
            if (getBit(input, start + i) != v) {
                setBit(input, start + i, v);
                inputsChanged = true;
            }
        }
        w.u16be(uint16_t(start));
        w.u16be(uint16_t(qty));
        break;
    }

    case fc::kWriteMultipleRegisters: {
        size_t start = r.u16be();
        size_t qty = r.u16be();
        size_t byteCount = r.u8();
        if (qty < 1 || qty > 123 || byteCount != qty * 2)
            return exception(function, ex::kIllegalDataValue);
        const uint8_t* data = r.take(byteCount);
        if (start + qty > inputRegisters())
            return exception(function, ex::kIllegalDataAddress);
        inputsChanged = std::memcmp(&input[start * 2], data, byteCount) != 0;
        std::memcpy(&input[start * 2], data, byteCount);
        w.u16be(uint16_t(start));
        w.u16be(uint16_t(qty));
        break;
    }

    case fc::kMaskWriteRegister: {
        size_t addr = r.u16be();
        uint16_t andMask = r.u16be();
        uint16_t orMask = r.u16be();
        if (addr >= inputRegisters())
            return exception(function, ex::kIllegalDataAddress);
        uint16_t cur = uint16_t((input[addr * 2] << 8) | input[addr * 2 + 1]);
        uint16_t next = uint16_t((cur & andMask) | (orMask & ~andMask));
        inputsChanged = next != cur;
        input[addr * 2] = uint8_t(next >> 8);
        input[addr * 2 + 1] = uint8_t(next);
        w.u16be(uint16_t(addr));
        w.u16be(andMask);
        w.u16be(orMask);
        break;
    }

    case fc::kReadWriteMultipleRegisters: {
        size_t readStart = r.u16be();
        size_t readQty = r.u16be();
        size_t writeStart = r.u16be();
        size_t writeQty = r.u16be();
        size_t byteCount = r.u8();
        if (readQty < 1 || readQty > 125 || writeQty < 1 || writeQty > 121 || byteCount != writeQty * 2)
            return exception(function, ex::kIllegalDataValue);
        const uint8_t* data = r.take(byteCount);
        if (writeStart + writeQty > inputRegisters())
            return exception(function, ex::kIllegalDataAddress);
        RegRef ref{};
        for (size_t i = 0; i < readQty; ++i)
            if (!holdingRef(readStart + i, false, ref))
                return exception(function, ex::kIllegalDataAddress);
        // The write is performed before the read (Modbus spec).
        inputsChanged = std::memcmp(&input[writeStart * 2], data, byteCount) != 0;
        std::memcpy(&input[writeStart * 2], data, byteCount);
        w.u8(uint8_t(readQty * 2));
        for (size_t i = 0; i < readQty; ++i) {
            holdingRef(readStart + i, false, ref);
            w.u8((*ref.area)[ref.byteOffset]);
            w.u8((*ref.area)[ref.byteOffset + 1]);
        }
        break;
    }

    case fc::kEncapsulatedInterface:
        return readDeviceId(r);

    default:
        return exception(function, ex::kIllegalFunction);
    }
    return std::move(w.data());
}

std::vector<uint8_t> ModbusSlave::Impl::readDeviceId(ByteReader& r)
{
    uint8_t mei = r.u8();
    if (mei != fc::kMeiReadDeviceId)
        return exception(fc::kEncapsulatedInterface, ex::kIllegalFunction);
    uint8_t code = r.u8();
    uint8_t objectId = r.u8();
    if (code < 1 || code > 4)
        return exception(fc::kEncapsulatedInterface, ex::kIllegalDataValue);

    const std::string* objects[3] = {&cfg.vendorName, &cfg.productCode, &cfg.revision};
    uint8_t first = 0, last = 2;
    if (code == 4) { // one specific object
        if (objectId > 2)
            return exception(fc::kEncapsulatedInterface, ex::kIllegalDataAddress);
        first = last = objectId;
    } else if (objectId <= 2) {
        first = objectId; // stream access starting at objectId (only basic objects exist)
    }

    ByteWriter w;
    w.u8(fc::kEncapsulatedInterface);
    w.u8(fc::kMeiReadDeviceId);
    w.u8(code);
    w.u8(0x81); // conformity: basic identification, stream + individual access
    w.u8(0);    // more follows: no
    w.u8(0);    // next object id
    w.u8(uint8_t(last - first + 1));
    for (uint8_t id = first; id <= last; ++id) {
        std::string value = objects[id]->substr(0, 64);
        w.u8(id);
        w.u8(uint8_t(value.size()));
        w.bytes(value.data(), value.size());
    }
    return std::move(w.data());
}

// ===========================================================================
// Public API
// ===========================================================================

ModbusSlave::ModbusSlave(ModbusSlaveConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
ModbusSlave::~ModbusSlave() = default;

bool ModbusSlave::start(std::string* error) { return impl_->start(error); }
void ModbusSlave::stop() { impl_->stop(); }

bool ModbusSlave::ioRead(size_t offset, void* data, size_t len) const
{
    if (offset > impl_->cfg.inputSize || len > impl_->cfg.inputSize - offset)
        return false;
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    std::memcpy(data, impl_->input.data() + offset, len);
    return true;
}

bool ModbusSlave::ioWrite(size_t offset, const void* data, size_t len)
{
    if (offset > impl_->cfg.outputSize || len > impl_->cfg.outputSize - offset)
        return false;
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    std::memcpy(impl_->output.data() + offset, data, len);
    return true;
}

std::vector<uint8_t> ModbusSlave::inputData() const
{
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    return std::vector<uint8_t>(impl_->input.begin(),
                                impl_->input.begin() + static_cast<std::ptrdiff_t>(impl_->cfg.inputSize));
}

size_t ModbusSlave::inputSize() const { return impl_->cfg.inputSize; }
size_t ModbusSlave::outputSize() const { return impl_->cfg.outputSize; }
bool ModbusSlave::masterConnected() const { return impl_->masterUp; }

std::vector<uint8_t> ModbusSlave::processPdu(const uint8_t* pdu, size_t len) { return impl_->processPdu(pdu, len); }

} // namespace softmb
