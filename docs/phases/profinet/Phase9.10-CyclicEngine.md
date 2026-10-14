# Phase 9.10: Cyclic RT_CLASS_1 data exchange

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
The provider sends the input CR (device -> controller) on time; the consumer parses the output CR and a data-hold watchdog ends the AR when the controller's frames stop.

## Documentation References
- [Wireshark PROFINET dissectors](https://gitlab.com/wireshark/wireshark/-/tree/master/plugins/epan/profinet) (`packet-pn-rt.c`)

---

## Step 9.10.1: `pn_cyclic.hpp`

**File:** `src/pn/pn_cyclic.hpp`

### Purpose
The C_SDU layout comes from the controller (IOCRBlockReq frame offsets), so `CyclicSetup` stores positions, not a fixed format.

### Implementation

```cpp
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
```

### Key Concepts
1. **Frame**: `FrameID | C_SDU | CycleCounter | DataStatus | TransferStatus`.
2. **IOPS / IOCS**: one status byte per submodule and direction; 0x80 = GOOD.
3. **`nextDue()`**: the cyclic thread sleeps exactly until the next frame is due (Phase 9.05 results).

---

## Step 9.10.2: `pn_cyclic.cpp`

**File:** `src/pn/pn_cyclic.cpp`

### Purpose
Frame building (data, IOPS, IOCS, cycle counter in 31.25 us units), period statistics, consumer evaluation (provider RUN, data valid, IOPS) and the watchdog.

### Implementation

```cpp
#include "pn_cyclic.hpp"

#include "pn_defs.hpp"

#include <algorithm>
#include <cstring>

namespace softpn {

CyclicEngine::CyclicEngine(size_t inputSize, size_t outputSize, Hooks hooks)
    : hooks_(std::move(hooks)), input_(inputSize, 0), output_(outputSize, 0)
{
}

void CyclicEngine::start(const CyclicSetup& setup)
{
    std::lock_guard<std::mutex> lock(mutex_);
    setup_ = setup;
    appReady_ = false;
    watchdogArmed_ = false;
    haveRx_ = false;
    cycleCounter_ = 0;
    nextSend_ = Clock::now();
    lastSend_ = Clock::time_point{};
    active_ = true;
}

void CyclicEngine::stop()
{
    bool wasRun = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        appReady_ = false;
        watchdogArmed_ = false;
        wasRun = run_.exchange(false);
    }
    if (wasRun && hooks_.onOutputs)
        hooks_.onOutputs(outputData(), false);
}

void CyclicEngine::setApplicationReady(bool ready)
{
    std::lock_guard<std::mutex> lock(mutex_);
    appReady_ = ready;
}

void CyclicEngine::armWatchdog()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!watchdogArmed_) {
        watchdogArmed_ = true;
        if (!haveRx_)
            lastRx_ = Clock::now(); // grace period: one full data-hold time from now
    }
}

bool CyclicEngine::setInputData(const void* data, size_t len)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (len > input_.size())
        return false;
    std::memcpy(input_.data(), data, len);
    return true;
}

std::vector<uint8_t> CyclicEngine::outputData() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return output_;
}

CyclicEngine::Clock::time_point CyclicEngine::nextDue() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ ? nextSend_ : Clock::time_point::max();
}

CyclicStats CyclicEngine::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::vector<uint8_t> CyclicEngine::buildInputFrame()
{
    const CyclicSetup& s = setup_;
    ByteWriter w;
    writeEthHeader(w, s.controllerMac, s.ownMac, kEtherTypeProfinet, s.vlan, kVlanTciRt);
    w.u16be(s.inputFrameId);
    const size_t csdu = w.size();
    w.zeros(s.inputDataLength);
    std::vector<uint8_t>& f = w.data();
    if (s.inputDataOffset >= 0 && size_t(s.inputDataOffset) + s.inputDataLen <= s.inputDataLength)
        std::memcpy(&f[csdu + size_t(s.inputDataOffset)], input_.data(), std::min<size_t>(s.inputDataLen, input_.size()));
    for (const StatusByte& b : s.inputIops)
        if (b.offset < s.inputDataLength)
            f[csdu + b.offset] = (appReady_ && b.proper) ? kIoxsGood : kIoxsBad;
    for (const StatusByte& b : s.inputIocs)
        if (b.offset < s.inputDataLength)
            f[csdu + b.offset] = b.proper ? kIoxsGood : kIoxsBad;
    cycleCounter_ = uint16_t(cycleCounter_ + s.cycleCounterIncrement);
    w.u16be(cycleCounter_);
    w.u8(kDataStatusDefault);
    w.u8(0); // TransferStatus
    std::vector<uint8_t> frame = std::move(f);
    padFrame(frame);
    return frame;
}

void CyclicEngine::tick(Clock::time_point now)
{
    std::vector<uint8_t> frame;
    bool expired = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_)
            return;
        if (watchdogArmed_ && setup_.dataHoldUs > 0 &&
            now - lastRx_ > std::chrono::microseconds(setup_.dataHoldUs)) {
            expired = true;
            watchdogArmed_ = false;
        }
        if (now >= nextSend_) {
            const auto period = std::chrono::microseconds(setup_.periodUs);
            if (lastSend_ != Clock::time_point{}) {
                const auto actual = std::chrono::duration_cast<std::chrono::microseconds>(now - lastSend_).count();
                stats_.maxTxPeriodUs = std::max<uint32_t>(stats_.maxTxPeriodUs, uint32_t(actual));
                if (actual * 2 > int64_t(setup_.periodUs) * 3)
                    ++stats_.lateCycles;
            }
            lastSend_ = now;
            nextSend_ += period;
            if (nextSend_ <= now) // fell behind (thread starved): resync instead of bursting
                nextSend_ = now + period;
            frame = buildInputFrame();
            ++stats_.txFrames;
        }
    }
    if (!frame.empty())
        hooks_.send(frame);
    if (expired && hooks_.onDataHoldExpired)
        hooks_.onDataHoldExpired();
}

bool CyclicEngine::onFrame(const EthHeader& eth, const uint8_t* frame, size_t len)
{
    std::vector<uint8_t> changedData;
    bool notify = false;
    bool runNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_)
            return false;
        const CyclicSetup& s = setup_;
        if (frameIdOf(frame, len, eth) != s.outputFrameId || eth.src != s.controllerMac)
            return false;
        const size_t csdu = eth.payloadOffset + 2;
        if (len < csdu + s.outputDataLength + 4)
            return true; // ours but truncated: ignore
        const uint8_t* p = frame + csdu;
        const uint16_t counter = uint16_t((p[s.outputDataLength] << 8) | p[s.outputDataLength + 1]);
        const uint8_t dataStatus = p[s.outputDataLength + 2];
        if (haveRx_ && counter == lastRxCounter_)
            return true; // duplicate
        haveRx_ = true;
        lastRxCounter_ = counter;
        lastRx_ = Clock::now();
        ++stats_.rxFrames;

        const bool providerRun = (dataStatus & kDataStatusRun) != 0;
        const bool valid = (dataStatus & kDataStatusValid) != 0;
        bool iopsGood = false;
        if (s.outputIopsOffset >= 0 && size_t(s.outputIopsOffset) < s.outputDataLength)
            iopsGood = (p[s.outputIopsOffset] & kIoxsGood) != 0;
        runNow = providerRun && valid && iopsGood && s.outputDataOffset >= 0;

        bool dataChanged = false;
        if (runNow && size_t(s.outputDataOffset) + s.outputDataLen <= s.outputDataLength) {
            const size_t n = std::min<size_t>(s.outputDataLen, output_.size());
            if (std::memcmp(output_.data(), p + s.outputDataOffset, n) != 0) {
                std::memcpy(output_.data(), p + s.outputDataOffset, n);
                dataChanged = true;
            }
        }
        const bool runChanged = run_.exchange(runNow) != runNow;
        if (dataChanged || runChanged) {
            notify = true;
            changedData = output_;
        }
    }
    if (notify && hooks_.onOutputs)
        hooks_.onOutputs(changedData, runNow);
    return true;
}

} // namespace softpn
```

### Key Concepts
1. **No burst after a stall**: if the thread falls behind, the schedule is resynchronised instead of sending several frames back to back.
2. **Watchdog grace**: armed at ApplicationReady (or the first frame), then `DataHoldFactor x period`.
3. **Callbacks without locks**: `onOutputs` / `onDataHoldExpired` are called after the mutex is released.

---

## Step 9.10.3: Add the cyclic engine

**File:** `CMakeLists.txt`

### Purpose
Append to the lists:

### Implementation

```cmake
src/pn/pn_cyclic.cpp      # SOFTPN_SOURCES
src/pn/pn_cyclic.hpp      # SOFTPN_HEADERS
```

### Key Concepts
1. **Thread**: driven by the device's cyclic thread (Phase 9.13).

---

## Verification Checklist

- [ ] `softpn` builds with 0 warnings
- [ ] Phase 9.15: cyclic echo > 60 frames/s at 8 ms, device max TX period < 9 ms over the memory cable

---

## Next Phase
Continue to [Phase9.11-RtaAlarms.md](Phase9.11-RtaAlarms.md): Alarm transport (RTA).
