// Software Modbus slave (server) over Modbus TCP and Modbus UDP.
//
// Process image, Hilscher-style PC view: the application only sees two byte areas.
//   input  area  PLC -> PC   written by the master, read by the app  (ioRead)
//   output area  PC -> PLC   written by the app, read by the master  (ioWrite)
//
// Modbus mapping (register k = area bytes [2k] high, [2k+1] low, i.e. Modbus big-endian
// on the wire is the same byte order as in the area):
//   holding registers  FC03/06/16/22/23  -> input  area (read/write)
//   coils              FC01/05/15        -> input  area, bit i = byte i/8, bit i%8
//   input registers    FC04              -> output area (read only)
//   discrete inputs    FC02              -> output area, bit view
//   optional: output area mirrored read-only into the holding table at
//   outputsInHoldingAt, for masters that only speak FC03/FC16.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace softmb {

struct ModbusSlaveConfig {
    std::string bindAddress = "0.0.0.0"; // NIC to listen on
    uint16_t port = 502;                 // Windows needs no admin rights for 502
    bool enableTcp = true;
    bool enableUdp = true;

    // 0 = answer any unit id. Otherwise answer this id plus 0xFF / 0 (the values
    // masters use to address a Modbus TCP device directly). Others get no reply.
    uint8_t unitId = 0;

    size_t inputSize = 64;  // bytes PLC -> PC (holding registers / coils)
    size_t outputSize = 64; // bytes PC -> PLC (input registers / discrete inputs)

    // >= 0: output area also readable (FC03) as holding registers starting here.
    int outputsInHoldingAt = -1;

    size_t maxTcpClients = 8;
    uint32_t tcpIdleTimeoutMs = 60000; // drop silent TCP clients
    uint32_t masterTimeoutMs = 3000;   // "master connected" = TCP client open or request within this time

    // FC 43 / 14 Read Device Identification (basic objects)
    std::string vendorName = "SoftFieldbus";
    std::string productCode = "SoftMB";
    std::string revision = "1.0";

    // Raise the network thread to TIME_CRITICAL on Windows.
    bool raiseThreadPriority = true;

    // Callbacks run on the network thread; keep them short.
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>& inputs)> onInputsChanged; // the master wrote new data
    std::function<void(bool masterConnected)> onConnectionChanged;
};

class ModbusSlave {
public:
    explicit ModbusSlave(ModbusSlaveConfig config);
    ~ModbusSlave();
    ModbusSlave(const ModbusSlave&) = delete;
    ModbusSlave& operator=(const ModbusSlave&) = delete;

    // Opens the TCP/UDP sockets and starts the network thread.
    bool start(std::string* error = nullptr);
    void stop();

    // Thread-safe process image access. Return false when offset+len is out of range.
    bool ioRead(size_t offset, void* data, size_t len) const;        // input area  (PLC -> PC)
    bool ioWrite(size_t offset, const void* data, size_t len);       // output area (PC -> PLC)
    std::vector<uint8_t> inputData() const;                          // whole input area
    size_t inputSize() const;
    size_t outputSize() const;

    bool masterConnected() const;

    // Executes one Modbus PDU (function code + data) against the process image and
    // returns the response PDU. Transport independent: TCP, UDP and a future RTU use it.
    std::vector<uint8_t> processPdu(const uint8_t* pdu, size_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace softmb
