// Software PROFINET RT IO-Device (Conformance Class A, RT_CLASS_1) on a standard PC NIC.
//
// The PLC is the IO-Controller: it finds the device by its NameOfStation (DCP), connects
// (RPC Connect / PrmEnd / ApplicationReady) and then exchanges cyclic I/O every 8..512 ms.
//
//   softpn::DeviceConfig cfg;
//   cfg.adapter = "Ethernet 2";            // or the NIC's MAC / IPv4 / Npcap name
//   cfg.stationName = "vision-station-1";
//   cfg.inputSize = 32;                    // PC -> PLC   (GSDML slot 1, "Input 32 bytes")
//   cfg.outputSize = 32;                   // PLC -> PC   (GSDML slot 2, "Output 32 bytes")
//   softpn::Device dev(cfg);
//   dev.start();
//   dev.setInputData(in, 32);
//   auto out = dev.outputData();
//
// Names follow the PLC's view, as in softeip: "input" = data to the PLC, "output" = from the PLC.
// Windows needs Npcap (https://npcap.com); Linux needs CAP_NET_RAW.
#pragma once

#include "softeip/export.hpp"
#include "softpn/l2_port.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace softpn {

enum class DeviceState {
    Stopped,
    WaitingForController, // started, no AR
    Connecting,           // AR established, parameterisation (Connect .. ApplicationReady)
    DataRun,              // cyclic exchange, controller in RUN
    DataStop,             // cyclic exchange, controller in STOP / outputs not valid
};

struct DeviceConfig {
    std::string adapter;               // NIC: Npcap name, friendly name ("Ethernet 2"), MAC or IPv4
    std::string stationName;           // default NameOfStation when none is persisted ("" = unnamed)
    std::string persistPath = "softpn.ini"; // NameOfStation set by the controller survives restarts
    uint16_t vendorId = 0xFFFF;        // PLACEHOLDER until Univision has a PI vendor ID; must match the GSDML
    uint16_t deviceId = 0x0001;        // must match the GSDML
    std::string orderId = "SOFTPN-1";              // I&M0, max 20 characters
    std::string serialNumber = "0000000000000001"; // I&M0, max 16 characters
    uint16_t hardwareRevision = 1;
    uint8_t softwareRevision[3] = {0, 1, 0};       // I&M0 "V0.1.0"
    std::string deviceVendorValue = "Univision SoftPN"; // DCP TypeOfStation
    size_t inputSize = 32;             // PC -> PLC: 16, 32, 64, 128 or 256 bytes
    size_t outputSize = 32;            // PLC -> PC: 16, 32, 64, 128 or 256 bytes
    uint32_t minUpdateTimeMs = 8;      // Connect requests with a faster update time are refused
    bool sendVlanTag = true;           // RT and alarm frames with 802.1Q priority 6
    bool promiscuous = false;          // see L2Options::promiscuous
    bool raiseThreadPriority = true;   // RX and cyclic threads at TIME_CRITICAL
    uint16_t rpcPort = 34964;          // tests only: another port when a controller runs on this PC
    std::shared_ptr<L2Port> port;      // tests only: use this port instead of opening `adapter`

    // Callbacks run on the stack's threads: keep them short and never call stop() from one.
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>& outputs, bool run)> onOutputs; // PLC wrote new data
    std::function<void(DeviceState)> onStateChanged;
    std::function<void()> onSignal; // DCP "flash LED" request (identify the device on site)
};

struct DeviceStats {
    uint64_t txFrames = 0;
    uint64_t rxFrames = 0;
    uint64_t lateCycles = 0;
    uint32_t maxTxPeriodUs = 0;
    uint64_t arConnects = 0;
    uint64_t arAborts = 0;
    std::string lastAbort;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // pimpl unique_ptr member of an exported class
#endif

class SOFTPN_API Device {
public:
    explicit Device(DeviceConfig config);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool start(std::string* error = nullptr);
    void stop();

    bool setInputData(const void* data, size_t len); // PC -> PLC, thread safe
    std::vector<uint8_t> outputData() const;         // PLC -> PC, thread safe (last valid values)
    bool controllerRun() const;
    DeviceState state() const;
    std::string stationName() const;
    DeviceStats stats() const;
    const DeviceConfig& config() const;

    static const char* stateName(DeviceState s);
    // "name | friendly name | MAC | IPv4" per adapter (empty + error text if Npcap is missing).
    static std::vector<std::string> listAdapters(std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace softpn
