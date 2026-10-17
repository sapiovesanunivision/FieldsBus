// Application Relation (AR) state machine: the PROFINET "connection" between the IO-Controller
// and this IO-Device, driven by RPC calls on the PNIO device interface.
//
//  Idle --Connect--> WaitPrmEnd --Control(PrmEnd)--> WaitAppReadyCnf --ApplicationReady.cnf--> Data
//    ^                   | Write (records)              | device sends ApplicationReady (retry 1 s)
//    +---- Release / abort (watchdog, RTA ERR, timeout, stop) <------------------------------+
//
// One AR at a time (IOCARSingle). Cyclic data and alarms are handled by CyclicEngine / RtaEngine;
// the AR only configures, starts and stops them through the hooks.
#pragma once

#include "pn_blocks.hpp"
#include "pn_cyclic.hpp"
#include "pn_model.hpp"
#include "pn_rpc.hpp"
#include "pn_rta.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace softpn {

enum class ArState { Idle, WaitPrmEnd, WaitAppReadyCnf, Data };
const char* arStateName(ArState s);

struct ArConfig {
    MacAddress mac;
    uint16_t vendorId = 0;
    uint16_t deviceId = 0;
    uint32_t minUpdateTimeMs = 8;
    bool vlan = true;
    uint16_t rpcPort = 34964;
    uint16_t controllerRpcPort = 34964; // ApplicationReady target port
    Im0Data im0;
};

class ArManager {
public:
    struct Hooks {
        std::function<void(const std::string&)> log;
        std::function<std::string()> stationName;
        std::function<void(const CyclicSetup&)> startCyclic;
        std::function<void()> stopCyclic;
        std::function<void(bool)> setApplicationReady;
        std::function<void()> armWatchdog;
        std::function<void(const RtaSetup&)> startRta;
        std::function<void()> stopRta;
        std::function<void(const PnioStatus&)> sendRtaError;
        std::function<bool(const UdpAddress& to, const Uuid& object, const Uuid& interfaceId, uint16_t opnum,
                           const Uuid& activity, uint32_t seqnum, const std::vector<uint8_t>& body)>
            sendRpcRequest;
        std::function<void(ArState)> stateChanged; // called with the AR lock held: keep it short
    };

    ArManager(ArConfig config, const DeviceModel& model, Hooks hooks);

    // RPC thread: a request on the PNIO device interface. Returns false to reject it.
    bool handleRequest(const RpcRequest& request, std::vector<uint8_t>& responseBody);
    // RPC thread: a response to our ApplicationReady request.
    void handleResponse(const RpcHeader& header, const std::vector<uint8_t>& body, const UdpAddress& from);
    // RPC thread, every few ms: ApplicationReady (re)transmission and timeouts.
    void poll(std::chrono::steady_clock::time_point now);
    // Any thread: ends the AR. reasonCode2 is the RTA ErrorCode2 (err::kRtaAbort...).
    void abort(uint8_t reasonCode2, bool notifyController, const std::string& why);

    ArState state() const { return state_.load(); }
    uint64_t connects() const { return connects_.load(); }
    uint64_t aborts() const { return aborts_.load(); }
    std::string lastAbort() const;

private:
    using Clock = std::chrono::steady_clock;

    std::vector<uint8_t> onConnect(const RpcRequest& req, const NdrArgs& args);
    std::vector<uint8_t> onRelease(const NdrArgs& args);
    std::vector<uint8_t> onControl(const NdrArgs& args);
    std::vector<uint8_t> onWrite(const NdrArgs& args);
    std::vector<uint8_t> onRead(const NdrArgs& args, bool implicit);
    PnioStatus writeRecord(const RecordHeader& h, const uint8_t* data, size_t len);
    void logRecord(const char* what, const RecordHeader& h, size_t len, const PnioStatus& st);
    PnioStatus validateConnect(const ConnectRequest& c) const;
    void sendApplicationReady();
    void setState(ArState s);
    void abortLocked(uint8_t reasonCode2, bool notifyController, const std::string& why);
    void log(const std::string& m) const
    {
        if (hooks_.log)
            hooks_.log(m);
    }

    ArConfig cfg_;
    const DeviceModel& model_;
    Hooks hooks_;
    mutable std::mutex mutex_;
    std::atomic<ArState> state_{ArState::Idle};
    std::atomic<uint64_t> connects_{0};
    std::atomic<uint64_t> aborts_{0};
    std::string lastAbort_;

    // Active AR (valid when state_ != Idle).
    Uuid arUuid_;
    uint16_t sessionKey_ = 0;
    Uuid initiatorObject_;
    UdpAddress controllerRpc_;
    Clock::time_point deadline_{};    // CMInitiatorActivityTimeout until Data
    Uuid appReadyActivity_;
    Clock::time_point appReadyNext_{};
    unsigned appReadyTries_ = 0;
};

} // namespace softpn
