// Software EtherNet/IP *scanner* (originator, the PLC side): opens one class-1 exclusive-owner connection to an
// adapter and exchanges cyclic I/O with it. Used to simulate a PLC (eip_scanner_sim, the Dev.3 process-image
// PLC simulator) or to drive an EtherNet/IP device from a PC.
//
// Data, named from the scanner's point of view:
//   output  O->T  scanner -> adapter   ioWrite()   sent every RPI with a 32-bit run/idle header
//   input   T->O  adapter -> scanner   ioRead()    modeless (no header)
// Assembly instances default to the Studio 5000 "Generic Ethernet Module" layout of softeip::Adapter
// (O->T 150, T->O 100, config 151).
//
// The network thread connects (ListIdentity, RegisterSession, Forward_Open), runs the cyclic exchange, and
// reconnects after a connection loss (no T->O data for 8 RPIs) or a rejected Forward_Open.
#pragma once

#include "softeip/export.hpp" // SOFTEIP_API
#include "softeip/eip_adapter.hpp" // IdentityInfo

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace softeip {

struct ScannerConfig {
    std::string target = "127.0.0.1"; // adapter IPv4 address
    uint16_t localIoPort = 2222;      // UDP port we receive T->O on; use another one (e.g. 2223) when the adapter
                                      // runs on the same PC, because it listens on 2222 itself
    uint32_t rpiUs = 10000;           // requested packet interval, both directions
    size_t outputSize = 32;           // O->T data bytes (without the run/idle header)
    size_t inputSize = 32;            // T->O data bytes
    uint16_t o2tInstance = 150;       // adapter's output assembly (consumes our outputs)
    uint16_t t2oInstance = 100;       // adapter's input assembly (produces our inputs)
    uint16_t configInstance = 151;
    unsigned reconnectDelayMs = 2000; // after a failed connect or a lost connection
    unsigned tcpTimeoutMs = 2000;     // TCP connect and explicit-message replies

    // Callbacks run on the network thread; keep them short.
    std::function<void(const std::string&)> onLog;
    std::function<void(bool connected)> onConnectionChanged;
    std::function<void(const uint8_t* inputs, size_t size)> onInputs; // every T->O packet
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // pimpl unique_ptr member of an exported class
#endif

class SOFTEIP_API Scanner {
public:
    explicit Scanner(ScannerConfig config);
    ~Scanner();
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    // Starts the network thread (it keeps reconnecting until stop()). Fails only for an invalid config.
    bool start(std::string* error = nullptr);
    // Stops the thread; sends Forward_Close and UnRegisterSession unless forwardClose is false (then the
    // adapter must time the connection out by itself).
    void stop(bool forwardClose = true);

    // Thread-safe I/O data access. Return false when offset+len is out of range.
    bool ioWrite(size_t offset, const void* data, size_t len);  // outputs O->T
    bool ioRead(size_t offset, void* data, size_t len) const;   // last received inputs T->O
    void setRun(bool run); // run/idle bit of the O->T header (default run)

    bool connected() const; // Forward_Open accepted and T->O data arriving

    struct Stats {
        uint64_t sent = 0;          // O->T packets
        uint64_t received = 0;      // T->O packets
        uint32_t connects = 0;      // accepted Forward_Opens
        uint32_t o2tId = 0, t2oId = 0;
        uint8_t lastOpenStatus = 0; // general status of the last Forward_Open (0 = ok)
        uint16_t lastOpenExtStatus = 0;
        bool forwardCloseOk = false;
        bool identityValid = false;
        IdentityInfo identity;      // from ListIdentity
        std::string productName;    // Identity object attribute 7
        std::string lastError;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace softeip
