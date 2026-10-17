// Minimal PROFINET IO-Controller, used to test softpn::Device without a PLC
// (pn_controller_sim on a NIC, pn_selftest over an in-memory cable).
//
// It does what a PLC does at startup, one step per method:
//   identify()   DCP Identify by NameOfStation -> device MAC and IP
//   setName()    DCP Set NameOfStation (optional)
//   connect()    RPC Connect with AR, 2 IOCRs, AlarmCR and the expected submodules
//   writeRecords(), readIm0()
//   prmEnd()     RPC Control(PrmEnd), then waits for the device's ApplicationReady and confirms it
//   exchange()   cyclic output frames + check that the device echoes them
//   release()    RPC Release
// It is a test tool: one AR, fixed slot layout matching the softpn GSDML, no PDev checks.
#pragma once

#include "pn_blocks.hpp"
#include "pn_frame.hpp"
#include "pn_rpc.hpp"
#include "softpn/l2_port.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace softpn {

struct ControllerConfig {
    std::shared_ptr<L2Port> port;          // NIC or one end of makeMemoryCable()
    std::string deviceName = "softpn-device";
    uint16_t localRpcPort = 34964;         // our RPC port (the device sends ApplicationReady there); another
                                           // port only when the device runs on the same host
    uint16_t deviceRpcPort = 34964;
    uint16_t vendorId = 0xFFFF;
    uint16_t deviceId = 0x0001;
    size_t inputSize = 32;                 // device -> controller (slot 1)
    size_t outputSize = 32;                // controller -> device (slot 2)
    uint16_t reductionRatio = 8;           // update time = RR ms (SendClockFactor 32)
    uint16_t dataHoldFactor = 3;
    bool advancedStartup = false;          // ARProperties.StartupMode
    bool wrongModule = false;              // expect a wrong ident in slot 1 -> ModuleDiffBlock
    bool vlan = true;                      // output frames with 802.1Q priority tag (prio 6, VID 0)
    std::function<void(const std::string&)> log;
};

struct DeviceFound {
    MacAddress mac;
    uint32_t ip = 0;
    std::string name;
    uint16_t vendorId = 0;
    uint16_t deviceId = 0;
};

class ControllerSim {
public:
    explicit ControllerSim(ControllerConfig cfg);
    ~ControllerSim();
    ControllerSim(const ControllerSim&) = delete;
    ControllerSim& operator=(const ControllerSim&) = delete;

    bool start(std::string* error = nullptr); // opens the RPC port, starts the RX thread
    void stop();

    bool identify(int timeoutMs = 2000);
    bool setName(const MacAddress& mac, const std::string& name, bool permanent = true, int timeoutMs = 1000);
    // Returns the Connect PNIOStatus (ok() = AR established).
    PnioStatus connect(int timeoutMs = 2000);
    bool writeRecords(int timeoutMs = 1000);
    bool readIm0(Im0Data& out, int timeoutMs = 1000);
    bool prmEnd(int timeoutMs = 3000); // includes waiting for and confirming ApplicationReady
    // Sends output frames for `ms` milliseconds (changing the data every 50 ms) and counts echoes.
    void exchange(int ms);
    void stopCyclic() { cyclicOn_ = false; } // simulates a dead controller: the device watchdog fires
    bool release(int timeoutMs = 1000);
    // Second, independent AR on the same device (must be refused while one is active).
    PnioStatus connectSecondAr(int timeoutMs = 2000);

    const DeviceFound& device() const { return device_; }
    const std::vector<DiffModule>& moduleDiff() const { return diff_; }
    bool arServerBlockSeen() const { return arServerBlock_; }
    uint64_t framesSent() const { return framesSent_; }
    uint64_t framesReceived() const { return framesReceived_; }
    uint64_t echoMatches() const { return echoMatches_; }
    bool inputIopsGood() const { return inputIopsGood_; }
    bool rtaErrorReceived() const { return rtaError_; }

private:
    using Clock = std::chrono::steady_clock;

    void log(const std::string& m) const
    {
        if (cfg_.log)
            cfg_.log(m);
    }
    void rxLoop();
    void rpcLoop();
    void cyclicLoop();
    // Sends an RPC request to the device and waits for the response blocks.
    bool call(uint16_t opnum, const std::vector<uint8_t>& blocks, NdrArgs& res, int timeoutMs);
    std::vector<uint8_t> buildConnect(const Uuid& arUuid, uint16_t sessionKey);
    std::vector<uint8_t> buildOutputFrame();

    ControllerConfig cfg_;
    std::atomic<bool> running_{false};
    std::thread rxThread_, rpcThread_, cyclicThread_;
    RpcEndpoint rpc_;
    Uuid activity_ = Uuid::random();
    uint32_t seq_ = 0;

    std::mutex m_;
    std::condition_variable cv_;
    std::map<uint32_t, std::pair<RpcHeader, std::vector<uint8_t>>> responses_; // by seqnum
    uint32_t identifyXid_ = 0;
    bool identified_ = false;
    bool setDone_ = false;
    uint8_t setError_ = 0xFF;
    bool appReadyReceived_ = false;

    DeviceFound device_;
    Uuid arUuid_;
    uint16_t sessionKey_ = 1;
    std::vector<DiffModule> diff_;
    bool arServerBlock_ = false;

    // Cyclic
    std::atomic<bool> cyclicOn_{false};
    uint16_t inputFrameId_ = 0x8001;
    uint16_t outputFrameId_ = 0x8000;
    uint16_t inputCsdu_ = 0, outputCsdu_ = 0;
    uint16_t inDataOffset_ = 0, inIopsOffset_ = 0;
    uint16_t cycleCounter_ = 0;
    std::vector<uint8_t> outPattern_, prevPattern_;
    std::atomic<uint64_t> framesSent_{0}, framesReceived_{0}, echoMatches_{0};
    std::atomic<bool> inputIopsGood_{false};
    std::atomic<bool> rtaError_{false};
};

} // namespace softpn
