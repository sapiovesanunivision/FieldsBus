# Phase 9.11: Alarm transport (RTA)

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Keep the AlarmCR healthy: acknowledge alarm DATA from the controller, end the AR when the controller sends an ERR, and notify the controller with an ERR when the device aborts.

## Documentation References
- [Wireshark PROFINET dissectors](https://gitlab.com/wireshark/wireshark/-/tree/master/plugins/epan/profinet) (`packet-pn-rtc-one.c`: RTA PDUs)

---

## Step 9.11.1: `pn_rta.hpp`

**File:** `src/pn/pn_rta.hpp`

### Purpose
Two channels (high 0xFC01, low 0xFE01) with their own sequence numbers.

### Implementation

```cpp
// RTA: the acknowledged alarm transport of the AlarmCR (FrameID 0xFC01 high / 0xFE01 low).
//
// PDU: AlarmDstEndpoint(2) AlarmSrcEndpoint(2) PDUType(1) AddFlags(1) SendSeqNum(2) AckSeqNum(2)
//      VarPartLen(2) VarPart
//
// PoC scope: acknowledge DATA from the controller, abort the AR on a received ERR, send ERR
// when the device aborts. Sending process/diagnosis alarms (DATA from the device) is a stub.
#pragma once

#include "pn_frame.hpp"
#include "pn_rpc.hpp"

#include <cstdint>
#include <functional>
#include <mutex>

namespace softpn {

struct RtaSetup {
    MacAddress ownMac;
    MacAddress controllerMac;
    uint16_t localRef = 0;  // our AlarmCR endpoint (AlarmCRBlockRes.LocalAlarmReference)
    uint16_t remoteRef = 0; // controller's (AlarmCRBlockReq.LocalAlarmReference)
    uint16_t tagHigh = 0xC000;
    uint16_t tagLow = 0xA000;
    bool vlan = true;
};

class RtaEngine {
public:
    struct Hooks {
        std::function<bool(const std::vector<uint8_t>&)> send;          // required
        std::function<void(const PnioStatus&)> onErrorReceived;         // called without locks held
        std::function<void(const std::string&)> log;
    };

    explicit RtaEngine(Hooks hooks) : hooks_(std::move(hooks)) {}

    void start(const RtaSetup& setup);
    void stop();
    // Sends an RTA ERR (AR abort notification) on the low-priority channel.
    void sendError(const PnioStatus& status);
    // RX thread: alarm frames. Returns true if the frame was for our AlarmCR.
    bool onFrame(const EthHeader& eth, const uint8_t* frame, size_t len);

private:
    struct Channel {
        uint16_t sendSeq = 0xFFFF; // last SendSeqNum we used [verify] initial value
        uint16_t ackSeq = 0xFFFE;  // last SendSeqNum received from the controller
    };
    std::vector<uint8_t> buildPdu(bool high, uint8_t pduType, const Channel& ch, const uint8_t* var, size_t varLen) const;

    Hooks hooks_;
    std::mutex mutex_;
    bool active_ = false;
    RtaSetup setup_;
    Channel high_;
    Channel low_;
};

} // namespace softpn
```

### Key Concepts
1. **PoC scope**: process/diagnosis alarms from the device (DATA) are not sent yet.
2. **`[verify]`**: the initial sequence numbers, against a CODESYS capture.

---

## Step 9.11.2: `pn_rta.cpp`

**File:** `src/pn/pn_rta.cpp`

### Purpose
PDU build/parse, ACK for DATA, ERR handling.

### Implementation

```cpp
#include "pn_rta.hpp"

#include "pn_defs.hpp"

namespace softpn {

void RtaEngine::start(const RtaSetup& setup)
{
    std::lock_guard<std::mutex> lock(mutex_);
    setup_ = setup;
    high_ = Channel{};
    low_ = Channel{};
    active_ = true;
}

void RtaEngine::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
}

std::vector<uint8_t> RtaEngine::buildPdu(bool high, uint8_t pduType, const Channel& ch, const uint8_t* var,
                                         size_t varLen) const
{
    ByteWriter w;
    writeEthHeader(w, setup_.controllerMac, setup_.ownMac, kEtherTypeProfinet, setup_.vlan,
                   high ? setup_.tagHigh : setup_.tagLow);
    w.u16be(high ? kFrameIdAlarmHigh : kFrameIdAlarmLow);
    w.u16be(setup_.remoteRef); // AlarmDstEndpoint
    w.u16be(setup_.localRef);  // AlarmSrcEndpoint
    w.u8(pduType);
    w.u8(rta::kAddFlagsWindow1);
    w.u16be(ch.sendSeq);
    w.u16be(ch.ackSeq);
    w.u16be(uint16_t(varLen));
    if (varLen)
        w.bytes(var, varLen);
    std::vector<uint8_t> f = std::move(w.data());
    padFrame(f);
    return f;
}

void RtaEngine::sendError(const PnioStatus& status)
{
    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_)
            return;
        const uint8_t var[4] = {status.code, status.decode, status.code1, status.code2};
        frame = buildPdu(false, rta::kPduErr, low_, var, sizeof var);
    }
    hooks_.send(frame);
}

bool RtaEngine::onFrame(const EthHeader& eth, const uint8_t* frame, size_t len)
{
    std::vector<uint8_t> reply;
    bool gotError = false;
    PnioStatus errorStatus;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || eth.src != setup_.controllerMac)
            return false;
        const uint16_t frameId = frameIdOf(frame, len, eth);
        const bool high = frameId == kFrameIdAlarmHigh;
        try {
            ByteReader r(frame + eth.payloadOffset + 2, len - eth.payloadOffset - 2);
            const uint16_t dst = r.u16be();
            const uint16_t src = r.u16be();
            const uint8_t pduType = r.u8();
            r.u8(); // AddFlags
            const uint16_t sendSeq = r.u16be();
            const uint16_t ackSeq = r.u16be();
            const uint16_t varLen = r.u16be();
            const uint8_t* var = r.take(varLen);
            (void)ackSeq;
            if (dst != setup_.localRef || src != setup_.remoteRef)
                return false;
            Channel& ch = high ? high_ : low_;
            switch (pduType & 0x0F) {
            case 0x01: // DATA: acknowledge (the PoC does not evaluate controller alarms)
                ch.ackSeq = sendSeq;
                reply = buildPdu(high, rta::kPduAck, ch, nullptr, 0);
                break;
            case 0x04: // ERR: the controller aborts the AR
                if (varLen >= 4)
                    errorStatus = PnioStatus::make(var[0], var[1], var[2], var[3]);
                gotError = true;
                active_ = false;
                break;
            default:   // ACK / NACK of our own DATA: we send none in the PoC
                break;
            }
        } catch (const ParseError&) {
            return true;
        }
    }
    if (!reply.empty())
        hooks_.send(reply);
    if (gotError) {
        if (hooks_.log)
            hooks_.log("RTA: controller aborted the AR, status " + errorStatus.str());
        if (hooks_.onErrorReceived)
            hooks_.onErrorReceived(errorStatus);
    }
    return true;
}

} // namespace softpn
```

### Key Concepts
1. **Endpoints**: AlarmDstEndpoint = the controller's LocalAlarmReference, AlarmSrcEndpoint = ours.
2. **ERR var part** = the PNIOStatus, e.g. `CF 81 FD 05` (consumer data-hold expired).

---

## Step 9.11.3: Add RTA

**File:** `CMakeLists.txt`

### Purpose
Append to the lists:

### Implementation

```cmake
src/pn/pn_rta.cpp      # SOFTPN_SOURCES
src/pn/pn_rta.hpp      # SOFTPN_HEADERS
```

### Key Concepts
1. **Used by** the AR state machine (abort) and the RX thread.

---

## Verification Checklist

- [ ] `softpn` builds with 0 warnings
- [ ] Phase 9.15: after a watchdog abort the controller simulator receives an RTA ERR

---

## Next Phase
Continue to [Phase9.12-ArStateMachine.md](Phase9.12-ArStateMachine.md): AR state machine.
