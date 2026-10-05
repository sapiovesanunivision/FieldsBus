// Transport-independent fieldbus device, modelled on the Hilscher cifX channel API.
//
// The application only sees two fixed byte areas, named from the PC's point of view:
//   input  area  PLC -> PC   ioRead()   (cf. xChannelIORead)
//   output area  PC -> PLC   ioWrite()  (cf. xChannelIOWrite)
// PC and PLC cast these bytes to agreed data types. Switching fieldbus is a config change:
//
//   softfb::DeviceConfig cfg;
//   cfg.transport = softfb::Transport::ModbusTcp;   // or EtherNetIP, ModbusUdp, ModbusTcpUdp
//   cfg.inputSize = 64; cfg.outputSize = 64;
//   softfb::FieldbusDevice dev(cfg);
//   dev.start();
//   dev.ioRead(0, &fromPlc, sizeof fromPlc);
//   dev.ioWrite(0, &toPlc, sizeof toPlc);
//
// Where the areas live on each bus:
//   EtherNet/IP  input area  = O->T output assembly (default 150, 32-bit run/idle header)
//                output area = T->O input assembly  (default 100)
//   Modbus       input area  = holding registers / coils
//                output area = input registers / discrete inputs
#pragma once

#include "softeip/eip_adapter.hpp" // IdentityInfo (header has no socket includes)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace softfb {

enum class Transport {
    EtherNetIP,
    ModbusTcp,
    ModbusUdp,
    ModbusTcpUdp, // one slave answering on both TCP and UDP
};

enum class DeviceState {
    Stopped,
    WaitingForMaster, // started, no PLC talking to us yet (EtherNet/IP scanner / Modbus client); name kept for API compatibility
    ConnectedIdle,    // EtherNet/IP: connection open, PLC in program/idle mode
    ConnectedRun,     // data exchange active (Modbus has no idle state)
};

struct DeviceConfig {
    Transport transport = Transport::EtherNetIP;
    std::string bindAddress = "0.0.0.0";
    size_t inputSize = 32;  // bytes PLC -> PC
    size_t outputSize = 32; // bytes PC -> PLC
    bool raiseThreadPriority = true;

    struct EtherNetIpOptions {
        uint16_t plcToPcInstance = 150; // O->T assembly = input area
        uint16_t pcToPlcInstance = 100; // T->O assembly = output area
        uint16_t configInstance = 151;
        uint32_t minRpiUs = 2000;
        softeip::IdentityInfo identity;
    } eip;

    struct ModbusOptions {
        uint16_t port = 502;
        uint8_t unitId = 0;          // 0 = any
        int outputsInHoldingAt = -1; // >= 0: output area also readable via FC03 from this register
        std::string vendorName = "SoftFieldbus";
        std::string productCode = "SoftMB";
        std::string revision = "1.0";
    } modbus;

    // Callbacks run on the network thread; keep them short.
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>& inputs)> onInputsChanged; // the PLC wrote new data
    std::function<void(DeviceState)> onStateChanged;
};

class FieldbusDevice {
public:
    explicit FieldbusDevice(DeviceConfig config);
    ~FieldbusDevice();
    FieldbusDevice(const FieldbusDevice&) = delete;
    FieldbusDevice& operator=(const FieldbusDevice&) = delete;

    bool start(std::string* error = nullptr);
    void stop();

    // Thread safe. Return false when offset+len is outside the area.
    bool ioRead(size_t offset, void* data, size_t len) const;  // input area  (PLC -> PC)
    bool ioWrite(size_t offset, const void* data, size_t len); // output area (PC -> PLC)

    DeviceState state() const;
    Transport transport() const;
    size_t inputSize() const;
    size_t outputSize() const;

    static const char* transportName(Transport t);
    static const char* stateName(DeviceState s);
    // Accepts "eip", "ethernetip", "modbus-tcp", "modbus-udp", "modbus" (= TCP+UDP).
    static bool parseTransport(const std::string& text, Transport& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace softfb
