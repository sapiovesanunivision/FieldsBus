// Software EtherNet/IP adapter (I/O device / "slave"). The PLC is the scanner
// (master): it opens a class-1 connection and exchanges cyclic I/O with us.
//
// Assembly model (defaults match the Studio 5000 "Generic Ethernet Module"):
//   input  instance 100  T->O  adapter -> PLC   (modeless, no run/idle header)
//   output instance 150  O->T  PLC -> adapter   (32-bit run/idle header)
//   config instance 151  size 0
//   198 = input-only heartbeat, 199 = listen-only heartbeat
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "softeip/export.hpp" // SOFTEIP_API

namespace softeip {

struct IdentityInfo {
    uint16_t vendorId = 0xFFFF; // replace with your ODVA-assigned vendor ID
    uint16_t deviceType = 0x000C; // communications adapter
    uint16_t productCode = 1;
    uint8_t revisionMajor = 1;
    uint8_t revisionMinor = 1;
    uint32_t serialNumber = 0x00C0FFEE;
    std::string productName = "SoftEIP Adapter";
};

struct AdapterConfig {
    IdentityInfo identity;

    std::string bindAddress = "0.0.0.0"; // NIC to listen on

    uint16_t inputInstance = 100;
    uint16_t outputInstance = 150;
    uint16_t configInstance = 151;
    uint16_t inputOnlyInstance = 198;
    uint16_t listenOnlyInstance = 199;
    size_t inputSize = 32;  // bytes, T->O
    size_t outputSize = 32; // bytes, O->T

    // Windows is not an RTOS: refuse RPIs we cannot honour reliably.
    uint32_t minRpiUs = 2000;
    // Raise the network thread to TIME_CRITICAL on Windows.
    bool raiseThreadPriority = true;

    // Callbacks run on the network thread; keep them short.
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>& outputs, bool run)> onOutputs;
    std::function<void(bool ownerConnected)> onConnectionChanged;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // pimpl unique_ptr member of an exported class
#endif

class SOFTEIP_API Adapter {
public:
    explicit Adapter(AdapterConfig config);
    ~Adapter();
    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    // Opens the sockets and starts the network thread.
    bool start(std::string* error = nullptr);
    void stop();

    // Thread-safe access to the process image.
    void setInputData(const uint8_t* data, size_t size); // what the PLC reads
    std::vector<uint8_t> outputData() const;             // what the PLC wrote
    bool plcInRun() const;      // run/idle bit of the last O->T packet
    bool outputConnected() const; // an exclusive-owner connection is open

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace softeip
