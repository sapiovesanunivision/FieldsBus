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
