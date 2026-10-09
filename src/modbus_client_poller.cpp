// Modbus CLIENT, cyclic: polls a Modbus server's areas into an input image and writes an output image.
#include "softmb/modbus_client_poller.hpp"
#include "softmb/modbus_defs.hpp"

#include "softeip/periodic_timer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace softmb {

namespace {

using Clock = std::chrono::steady_clock;

bool isBitTable(Table t) { return t == Table::Coils || t == Table::DiscreteInputs; }

size_t areaBytes(const PollArea& a) { return isBitTable(a.table) ? (size_t(a.count) + 7) / 8 : size_t(a.count) * 2; }

const char* tableName(Table t)
{
    switch (t) {
    case Table::Coils: return "coils";
    case Table::DiscreteInputs: return "discrete inputs";
    case Table::HoldingRegisters: return "holding registers";
    case Table::InputRegisters: return "input registers";
    }
    return "?";
}

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

} // namespace

struct ModbusClientPoller::Impl {
    explicit Impl(ModbusClientPollerConfig c)
        : cfg(std::move(c)), input(cfg.inputSize, 0), output(cfg.outputSize, 0)
    {
        if (!cfg.client.onLog && cfg.onLog)
            cfg.client.onLog = cfg.onLog;
        written.assign(cfg.writes.size(), std::vector<uint8_t>());
        initExceptionLogged.assign(cfg.initWrites.size(), false);
        client = std::make_unique<ModbusClient>(cfg.client);
    }
    ~Impl() { stop(); }

    bool validate(std::string* error) const;
    bool start(std::string* error);
    void stop();
    void run();
    bool cycle(std::string& error);
    bool sendInitWrites(std::string& error); // caller holds writeMutex
    bool writeAreas(bool all, std::string& error, Result* first); // caller holds writeMutex
    Result readArea(const PollArea& a, std::vector<uint8_t>& image);
    Result writeArea(const PollArea& a, const uint8_t* bytes);
    void setOnline(bool up, const std::string& why);

    void log(const std::string& msg) const
    {
        if (cfg.onLog)
            cfg.onLog(msg);
    }

    ModbusClientPollerConfig cfg;
    std::unique_ptr<ModbusClient> client;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> up{false};

    mutable std::mutex dataMutex;
    std::vector<uint8_t> input;  // server -> PC, guarded by dataMutex
    std::vector<uint8_t> output; // PC -> server, guarded by dataMutex
    // Guards the write side (written, initPending, resendPending) and serializes the poll thread's
    // writes with flushOutputs() from the application thread.
    std::mutex writeMutex;
    std::vector<std::vector<uint8_t>> written; // per write area: bytes last written OK (empty = must write)
    bool initPending = true;    // initWrites must be sent before the next reads
    bool resendPending = false; // OnDemand + resendOutputsOnReconnect: a cycle failed since the last write
    std::vector<bool> initExceptionLogged;

    mutable std::mutex statsMutex;
    Stats stats;
};

bool ModbusClientPoller::Impl::validate(std::string* error) const
{
    auto fail = [&](const std::string& what) {
        if (error)
            *error = what;
        return false;
    };
    if (cfg.cycleMs == 0)
        return fail("cycleMs must be > 0");
    if (cfg.reads.empty() && cfg.writes.empty())
        return fail("no read or write areas configured");
    auto check = [&](const PollArea& a, size_t imageSize, const char* kind) {
        std::string where = std::string(kind) + " area " + tableName(a.table) + " @" + std::to_string(a.address);
        if (a.count == 0)
            return fail(where + ": count is 0");
        if (size_t(a.address) + a.count > 0x10000)
            return fail(where + ": address + count beyond 65535");
        if (a.imageOffset > imageSize || areaBytes(a) > imageSize - a.imageOffset)
            return fail(where + ": does not fit the " + kind + " image (offset " + std::to_string(a.imageOffset) +
                        " + " + std::to_string(areaBytes(a)) + " B > " + std::to_string(imageSize) + " B)");
        return true;
    };
    for (const auto& a : cfg.reads)
        if (!check(a, cfg.inputSize, "read"))
            return false;
    for (const auto& a : cfg.writes) {
        if (a.table != Table::Coils && a.table != Table::HoldingRegisters)
            return fail(std::string("write area @") + std::to_string(a.address) + ": only coils and holding registers are writable");
        if (!check(a, cfg.outputSize, "write"))
            return false;
    }
    return true;
}

bool ModbusClientPoller::Impl::start(std::string* error)
{
    if (running)
        return true;
    if (!validate(error))
        return false;
    {
        std::lock_guard<std::mutex> lock(writeMutex);
        initPending = true;
        resendPending = false;
        for (auto& w : written)
            w.clear();
    }
    running = true;
    thread = std::thread([this] { run(); });
    log("Modbus client poller started: server " + cfg.client.host + ":" + std::to_string(cfg.client.port) + " (" +
        (cfg.client.transport == ClientTransport::Tcp ? "TCP" : "UDP") + "), unit " + std::to_string(cfg.client.unitId) +
        ", cycle " + std::to_string(cfg.cycleMs) + " ms, " + std::to_string(cfg.reads.size()) + " read / " +
        std::to_string(cfg.writes.size()) + " write areas");
    return true;
}

void ModbusClientPoller::Impl::stop()
{
    running = false;
    client->abort(); // a request waiting for its reply returns at once
    if (thread.joinable())
        thread.join();
    client->close();
    up = false;
}

void ModbusClientPoller::Impl::setOnline(bool nowUp, const std::string& why)
{
    if (nowUp == up)
        return;
    up = nowUp;
    log(nowUp ? std::string("server online") : "server offline: " + why);
    if (cfg.onOnlineChanged)
        cfg.onOnlineChanged(nowUp);
}

Result ModbusClientPoller::Impl::readArea(const PollArea& a, std::vector<uint8_t>& image)
{
    const bool bits = isBitTable(a.table);
    const size_t chunkMax = bits ? limits::kReadBits : limits::kReadRegisters; // 2000 is a multiple of 8
    for (size_t done = 0; done < a.count;) {
        const uint16_t n = uint16_t(std::min<size_t>(chunkMax, a.count - done));
        const uint16_t addr = uint16_t(a.address + done);
        Result r;
        if (bits) {
            std::vector<bool> v;
            r = a.table == Table::Coils ? client->readCoils(addr, n, v) : client->readDiscreteInputs(addr, n, v);
            if (!r.ok())
                return r;
            uint8_t* base = image.data() + a.imageOffset + done / 8;
            for (size_t i = 0; i < n; ++i)
                setBit(base, i, v[i]);
        } else {
            std::vector<uint16_t> v;
            r = a.table == Table::HoldingRegisters ? client->readHoldingRegisters(addr, n, v)
                                                   : client->readInputRegisters(addr, n, v);
            if (!r.ok())
                return r;
            uint8_t* p = image.data() + a.imageOffset + done * 2;
            for (size_t i = 0; i < n; ++i) {
                p[i * 2] = uint8_t(v[i] >> 8);
                p[i * 2 + 1] = uint8_t(v[i]);
            }
        }
        done += n;
    }
    return {};
}

Result ModbusClientPoller::Impl::writeArea(const PollArea& a, const uint8_t* bytes)
{
    const bool bits = a.table == Table::Coils;
    const size_t chunkMax = bits ? limits::kWriteBits : limits::kWriteRegisters; // 1968 is a multiple of 8
    for (size_t done = 0; done < a.count;) {
        const size_t n = std::min<size_t>(chunkMax, a.count - done);
        const uint16_t addr = uint16_t(a.address + done);
        Result r;
        if (bits) {
            std::vector<bool> v(n);
            const uint8_t* base = bytes + done / 8;
            for (size_t i = 0; i < n; ++i)
                v[i] = getBit(base, i);
            r = client->writeMultipleCoils(addr, v);
        } else {
            std::vector<uint16_t> v(n);
            const uint8_t* p = bytes + done * 2;
            for (size_t i = 0; i < n; ++i)
                v[i] = uint16_t((p[i * 2] << 8) | p[i * 2 + 1]);
            r = client->writeMultipleRegisters(addr, v);
        }
        if (!r.ok())
            return r;
        done += n;
    }
    return {};
}

bool ModbusClientPoller::Impl::sendInitWrites(std::string& error)
{
    for (size_t i = 0; i < cfg.initWrites.size(); ++i) {
        const InitWrite& w = cfg.initWrites[i];
        Result r = client->writeSingleRegister(w.address, w.value);
        if (r.code == ResultCode::Exception) {
            if (!initExceptionLogged[i])
                log("init write register " + std::to_string(w.address) + " = " + std::to_string(w.value) +
                    " ignored: " + r.text());
            initExceptionLogged[i] = true;
            continue;
        }
        if (!r.ok()) {
            error = "init write register " + std::to_string(w.address) + ": " + r.text();
            return false;
        }
    }
    initPending = false;
    return true;
}

bool ModbusClientPoller::Impl::writeAreas(bool all, std::string& error, Result* first)
{
    std::vector<uint8_t> out;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        out = output;
    }
    for (size_t i = 0; i < cfg.writes.size(); ++i) {
        const PollArea& a = cfg.writes[i];
        const uint8_t* bytes = out.data() + a.imageOffset;
        std::vector<uint8_t> slice(bytes, bytes + areaBytes(a));
        if (!all && written[i] == slice)
            continue;
        Result r = writeArea(a, bytes);
        if (!r.ok()) {
            written[i].clear(); // write again after the error
            error = std::string("write ") + tableName(a.table) + " @" + std::to_string(a.address) + ": " + r.text();
            if (first)
                *first = r;
            return false;
        }
        written[i] = std::move(slice);
    }
    return true;
}

bool ModbusClientPoller::Impl::cycle(std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(writeMutex);
        if (initPending && !sendInitWrites(error))
            return false;
    }

    // Reads: into a copy, published only when all reads succeeded (no half-updated image).
    std::vector<uint8_t> next;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        next = input;
    }
    for (const auto& a : cfg.reads) {
        Result r = readArea(a, next);
        if (!r.ok()) {
            error = std::string("read ") + tableName(a.table) + " @" + std::to_string(a.address) + ": " + r.text();
            return false;
        }
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        changed = next != input;
        if (changed)
            input = next;
    }
    if (changed && cfg.onInputsChanged)
        cfg.onInputsChanged(next);

    // Writes
    std::lock_guard<std::mutex> lock(writeMutex);
    if (cfg.writeMode == WriteMode::OnDemand) {
        if (!(cfg.resendOutputsOnReconnect && resendPending))
            return true;
        if (!writeAreas(true, error, nullptr))
            return false;
        resendPending = false;
        return true;
    }
    return writeAreas(cfg.writeMode == WriteMode::EveryCycle, error, nullptr);
}

void ModbusClientPoller::Impl::run()
{
#ifdef _WIN32
    if (cfg.raiseThreadPriority)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    softeip::PeriodicTimer timer(std::chrono::milliseconds(cfg.cycleMs));
    Clock::time_point lastStart{};
    while (running) {
        const auto start = Clock::now();
        std::string error;
        const bool ok = cycle(error);
        const double cycleMs = msSince(start);
        if (!ok) {
            std::lock_guard<std::mutex> lock(writeMutex);
            for (auto& w : written)
                w.clear(); // after reconnecting, the server gets the current outputs again
            initPending = true;
            resendPending = true;
        }
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            ++stats.cycles;
            if (!ok) {
                ++stats.failedCycles;
                stats.lastError = error;
            }
            stats.lastCycleMs = cycleMs;
            stats.maxCycleMs = std::max(stats.maxCycleMs, cycleMs);
            if (lastStart != Clock::time_point{})
                stats.maxPeriodMs = std::max(stats.maxPeriodMs,
                                             std::chrono::duration<double, std::milli>(start - lastStart).count());
        }
        lastStart = start;
        setOnline(ok, error);
        timer.wait();
    }
}

// ===========================================================================
// Public API
// ===========================================================================

ModbusClientPoller::ModbusClientPoller(ModbusClientPollerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}
ModbusClientPoller::~ModbusClientPoller() = default;

bool ModbusClientPoller::start(std::string* error) { return impl_->start(error); }
void ModbusClientPoller::stop() { impl_->stop(); }

bool ModbusClientPoller::ioRead(size_t offset, void* data, size_t len) const
{
    if (offset > impl_->cfg.inputSize || len > impl_->cfg.inputSize - offset)
        return false;
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    std::memcpy(data, impl_->input.data() + offset, len);
    return true;
}

bool ModbusClientPoller::ioWrite(size_t offset, const void* data, size_t len)
{
    if (offset > impl_->cfg.outputSize || len > impl_->cfg.outputSize - offset)
        return false;
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    std::memcpy(impl_->output.data() + offset, data, len);
    return true;
}

Result ModbusClientPoller::flushOutputs()
{
    std::lock_guard<std::mutex> lock(impl_->writeMutex);
    std::string error;
    Result first;
    impl_->writeAreas(true, error, &first);
    return first;
}

std::vector<uint8_t> ModbusClientPoller::inputData() const
{
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    return impl_->input;
}

bool ModbusClientPoller::online() const { return impl_->up; }

ModbusClientPoller::Stats ModbusClientPoller::stats() const
{
    std::lock_guard<std::mutex> lock(impl_->statsMutex);
    return impl_->stats;
}

} // namespace softmb
