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
