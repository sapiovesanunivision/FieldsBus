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
