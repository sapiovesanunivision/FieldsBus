// Role: Modbus CLIENT (formerly "master"). The PC sends the requests; the PLC / device is the
// Modbus SERVER (formerly "slave") and holds the registers. This is the role of a PC that polls a
// PLC (or a Wago coupler, a gateway, EasyModbus Server Simulator, ...). For the opposite role
// (the PLC polls the PC) use softmb::ModbusServer. See README "Roles".
//
// Synchronous request API over Modbus TCP or Modbus UDP:
//   - one request at a time, thread-safe (calls from several threads are serialized)
//   - TCP: connects on the first request (or connect()), reconnects automatically after an error
//   - UDP: connectionless; a timed-out request is resent `retries` times
//   - addresses are Modbus protocol addresses (0-based); register values are host-order uint16_t
//   - no C++ exceptions: every call returns a Result
// For a cyclic process image (poll the PLC every N ms) see softmb::ModbusClientPoller.
#pragma once

#include "softeip/export.hpp" // SOFTMB_API

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace softmb {

enum class ClientTransport { Tcp, Udp };

struct ModbusClientConfig {
    std::string host = "127.0.0.1";  // the Modbus server (PLC): IPv4 address or host name
    uint16_t port = 502;
    ClientTransport transport = ClientTransport::Tcp;
    uint8_t unitId = 1;              // unit id of the server (some TCP devices/gateways want 0xFF)
    std::string bindAddress;         // optional local NIC address; empty = any

    unsigned connectTimeoutMs = 1000;  // TCP connect
    unsigned responseTimeoutMs = 500;  // per request attempt
    unsigned retries = 1;              // extra attempts after a timeout (UDP resend / TCP reconnect + resend)
    unsigned reconnectDelayMs = 1000;  // TCP: minimum time between connection attempts after a failure

    // Called from the calling thread on connection events (connected, lost, reconnecting); keep it short.
    std::function<void(const std::string&)> onLog;
};

enum class ResultCode {
    Ok,
    Exception,       // the server answered with a Modbus exception (Result::exception holds the code)
    Timeout,         // no (valid) answer within responseTimeoutMs, after all retries
    NotConnected,    // TCP connection could not be opened (or a reconnect is delayed)
    ProtocolError,   // malformed or unexpected answer
    InvalidArgument  // rejected locally (quantity 0 or above the protocol limit, address overflow, ...)
};

struct SOFTMB_API Result {
    ResultCode code = ResultCode::Ok;
    uint8_t exception = 0; // Modbus exception code when code == Exception
    bool ok() const { return code == ResultCode::Ok; }
    std::string text() const; // e.g. "ok", "exception 02 (illegal data address)", "timeout"
};

SOFTMB_API const char* resultCodeName(ResultCode code);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // pimpl unique_ptr member of an exported class
#endif

class SOFTMB_API ModbusClient {
public:
    explicit ModbusClient(ModbusClientConfig config);
    ~ModbusClient();
    ModbusClient(const ModbusClient&) = delete;
    ModbusClient& operator=(const ModbusClient&) = delete;

    // Optional: TCP connects (requests connect on demand anyway); UDP opens the local socket.
    bool connect(std::string* error = nullptr);
    void close();
    // Thread-safe, does not wait: a request waiting for its reply (or the next request) returns
    // NotConnected within ~50 ms instead of waiting for its timeout. A TCP connect in progress still
    // finishes (at most connectTimeoutMs). Cleared by close() and connect().
    void abort();
    // TCP: connection open. UDP: the last request was answered.
    bool connected() const;
    const ModbusClientConfig& config() const;

    // Bit tables
    Result readCoils(uint16_t address, uint16_t count, std::vector<bool>& values);          // FC01
    Result readDiscreteInputs(uint16_t address, uint16_t count, std::vector<bool>& values); // FC02
    Result writeSingleCoil(uint16_t address, bool value);                                   // FC05
    Result writeMultipleCoils(uint16_t address, const std::vector<bool>& values);           // FC15

    // Register tables
    Result readHoldingRegisters(uint16_t address, uint16_t count, std::vector<uint16_t>& values); // FC03
    Result readInputRegisters(uint16_t address, uint16_t count, std::vector<uint16_t>& values);   // FC04
    Result writeSingleRegister(uint16_t address, uint16_t value);                                 // FC06
    Result writeMultipleRegisters(uint16_t address, const std::vector<uint16_t>& values);         // FC16
    Result maskWriteRegister(uint16_t address, uint16_t andMask, uint16_t orMask);                // FC22
    Result readWriteMultipleRegisters(uint16_t readAddress, uint16_t readCount, uint16_t writeAddress,
                                      const std::vector<uint16_t>& writeValues,
                                      std::vector<uint16_t>& readValues);                         // FC23

    // Raw request: `request` is a full PDU (function code + data). On Ok or Exception, `response`
    // holds the response PDU. unitOverride >= 0 addresses another unit id for this request only.
    Result transact(const std::vector<uint8_t>& request, std::vector<uint8_t>& response, int unitOverride = -1);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace softmb
