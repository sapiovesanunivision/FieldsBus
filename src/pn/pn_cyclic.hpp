// Cyclic RT_CLASS_1 data exchange.
//
// Frame: Ethernet [VLAN] 0x8892 | FrameID | C_SDU (DataLength bytes) | CycleCounter(2) | DataStatus(1) | TransferStatus(1)
//
// Provider (input CR, device -> controller): sent every SendClockFactor x ReductionRatio x 31.25 us
//   from the 1 ms tick of the cyclic thread. The C_SDU layout (where each submodule's data, IOPS
//   and IOCS go) comes from the controller's IOCRBlockReq.
// Consumer (output CR, controller -> device): parsed on the RX thread; a data-hold watchdog
//   aborts the AR when frames stop arriving.
#pragma once

#include "pn_frame.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace softpn {

// One IOPS or IOCS byte in a C_SDU. `proper` = the submodule matched the expected configuration.
struct StatusByte {
    uint16_t offset = 0;
    bool proper = false;
};

struct CyclicSetup {
    MacAddress ownMac;
    MacAddress controllerMac;
    bool vlan = true;

    // Input CR (device -> controller).
    uint16_t inputFrameId = 0;
    uint16_t inputDataLength = 0;       // C_SDU length
    uint32_t periodUs = 0;              // SendClockFactor x ReductionRatio x 31.25 us
    uint16_t cycleCounterIncrement = 0; // SendClockFactor x ReductionRatio
    int inputDataOffset = -1;           // slot 1 data (-1: submodule not proper, nothing copied)
    uint16_t inputDataLen = 0;
    std::vector<StatusByte> inputIops;  // IOPS of every submodule we provide
    std::vector<StatusByte> inputIocs;  // our consumer status for the controller's output submodules

    // Output CR (controller -> device).
    uint16_t outputFrameId = 0;
    uint16_t outputDataLength = 0;
    uint32_t dataHoldUs = 0;            // DataHoldFactor x period of the output CR
    int outputDataOffset = -1;          // slot 2 data (-1: submodule not proper)
    uint16_t outputDataLen = 0;
    int outputIopsOffset = -1;
};

struct CyclicStats {
    uint64_t txFrames = 0;
    uint64_t rxFrames = 0;
    uint64_t lateCycles = 0;   // a period took more than 1.5 x nominal
    uint32_t maxTxPeriodUs = 0;
};

class CyclicEngine {
public:
    struct Hooks {
        std::function<bool(const std::vector<uint8_t>&)> send;                 // required
        std::function<void(const std::vector<uint8_t>& data, bool run)> onOutputs;
        std::function<void()> onDataHoldExpired;                               // called without locks held
    };

    CyclicEngine(size_t inputSize, size_t outputSize, Hooks hooks);

    void start(const CyclicSetup& setup); // after Connect.res
    void stop();                          // AR released / aborted: outputs become invalid
    void setApplicationReady(bool ready); // IOPS GOOD for proper submodules from PrmEnd on
    void armWatchdog();                   // start supervising the output CR

    bool setInputData(const void* data, size_t len); // PC -> PLC
    std::vector<uint8_t> outputData() const;         // PLC -> PC (last valid values)
    bool controllerRun() const { return run_.load(); } // provider RUN + data valid + IOPS GOOD
    bool active() const { return active_.load(); }
    CyclicStats stats() const;

    // When the next input frame is due (time_point::max() while no AR is active).
    std::chrono::steady_clock::time_point nextDue() const;
    // Cyclic thread: sends the input frame when due, checks the watchdog.
    void tick(std::chrono::steady_clock::time_point now);
    // RX thread: an output CR frame (FrameID in the RT_CLASS_1 range). Returns true if it was ours.
    bool onFrame(const EthHeader& eth, const uint8_t* frame, size_t len);

private:
    using Clock = std::chrono::steady_clock;
    std::vector<uint8_t> buildInputFrame(); // caller holds mutex_

    Hooks hooks_;
    mutable std::mutex mutex_;
    CyclicSetup setup_;
    std::atomic<bool> active_{false};
    std::atomic<bool> run_{false};
    bool appReady_ = false;
    bool watchdogArmed_ = false;
    std::vector<uint8_t> input_;
    std::vector<uint8_t> output_;
    uint16_t cycleCounter_ = 0;
    Clock::time_point nextSend_{};
    Clock::time_point lastSend_{};
    Clock::time_point lastRx_{};
    bool haveRx_ = false;
    uint16_t lastRxCounter_ = 0;
    CyclicStats stats_;
};

} // namespace softpn
