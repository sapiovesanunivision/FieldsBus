// Role: Modbus CLIENT (formerly "master"), cyclic. The PC polls a PLC / device that is the Modbus
// SERVER (holds the registers), the way UvcIOModBus apps poll a Wago coupler or a PLC: every cycleMs
// the poller reads the configured server areas into an INPUT image and writes the OUTPUT image to
// the configured server areas. The application only uses ioRead / ioWrite. For the opposite role
// (the PLC polls the PC) use softmb::ModbusServer. See README "Roles".
//
// Process image (same PC view and byte layout as softmb::ModbusServer, so code can switch roles):
//   input  image  server -> PC  (ioRead):  filled from `reads`  (any table)
//   output image  PC -> server  (ioWrite): sent to   `writes` (coils or holding registers)
//   registers: 2 bytes each, big-endian (register k of an area <-> bytes [2k], [2k+1] from imageOffset)
//   bits:      LSB first (bit i of an area <-> byte imageOffset + i/8, bit i%8)
// Areas larger than the protocol limits are split into several requests automatically.
#pragma once

#include "softmb/modbus_client.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace softmb {

enum class Table { Coils, DiscreteInputs, HoldingRegisters, InputRegisters };

struct PollArea {
    Table table = Table::HoldingRegisters;
    uint16_t address = 0;   // first server address (0-based protocol address)
    uint16_t count = 0;     // registers or bits
    size_t imageOffset = 0; // byte offset in the input image (reads) or output image (writes)
};

enum class WriteMode {
    OnChange,   // write an area only when its bytes changed (and again after every communication error)
    EveryCycle, // write every area every cycle
    OnDemand    // the poll thread never writes on its own; the application calls flushOutputs()
                // (see resendOutputsOnReconnect). Used to replace UvcIOModBus, which sent each write at once.
};

// A single register write (FC06) sent before the first cycle and again before the first cycle after
// every communication error, e.g. a coupler watchdog reset. A Modbus exception reply is logged and
// ignored (the server may not have that register); a timeout or lost connection fails the cycle.
struct InitWrite {
    uint16_t address = 0;
    uint16_t value = 0;
};

struct ModbusClientPollerConfig {
    ModbusClientConfig client;     // server address, transport, unit id, timeouts
    unsigned cycleMs = 100;        // poll period
    size_t inputSize = 0;          // input image bytes  (server -> PC)
    size_t outputSize = 0;         // output image bytes (PC -> server)
    std::vector<PollArea> reads;   // any table
    std::vector<PollArea> writes;  // Table::Coils or Table::HoldingRegisters
    WriteMode writeMode = WriteMode::OnChange;
    // OnDemand only: after a communication error, write the whole output image again in the first
    // good cycle. Off by default: outputs set while the server was unreachable are not replayed.
    bool resendOutputsOnReconnect = false;
    std::vector<InitWrite> initWrites;
    bool raiseThreadPriority = false;

    // Called on the poll thread; keep them short.
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>& inputs)> onInputsChanged; // the server's data changed
    std::function<void(bool online)> onOnlineChanged; // false after a failed cycle, true after a good one
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // pimpl unique_ptr member of an exported class
#endif

class SOFTMB_API ModbusClientPoller {
public:
    explicit ModbusClientPoller(ModbusClientPollerConfig config);
    ~ModbusClientPoller();
    ModbusClientPoller(const ModbusClientPoller&) = delete;
    ModbusClientPoller& operator=(const ModbusClientPoller&) = delete;

    // Validates the configuration and starts the poll thread (it keeps retrying if the server is down).
    bool start(std::string* error = nullptr);
    void stop();

    // Thread-safe process image access. Return false when offset+len is out of range.
    bool ioRead(size_t offset, void* data, size_t len) const;   // input image  (server -> PC)
    bool ioWrite(size_t offset, const void* data, size_t len);  // output image (PC -> server)
    // Writes every write area now, from the calling thread (one request per area chunk), and
    // returns the first error. Serialized with the poll thread's writes. Works in every WriteMode.
    Result flushOutputs();
    std::vector<uint8_t> inputData() const;

    // true while the last cycle completed (all reads and writes answered).
    bool online() const;

    struct Stats {
        uint64_t cycles = 0;
        uint64_t failedCycles = 0;
        double lastCycleMs = 0;  // duration of the last cycle's Modbus traffic
        double maxCycleMs = 0;
        double maxPeriodMs = 0;  // longest time between two cycle starts (jitter check)
        std::string lastError;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace softmb
