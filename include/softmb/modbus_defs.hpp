// Modbus protocol definitions shared by the Modbus SERVER (softmb::ModbusServer) and the
// Modbus CLIENT (softmb::ModbusClient): function codes, exception codes, protocol limits,
// MBAP sizes and the LSB-first bit packing used by FC01/02/05/15.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace softmb {

constexpr size_t kMbapSize = 7; // transaction id, protocol id, length, unit id
constexpr size_t kMaxPdu = 253;
constexpr size_t kMaxAdu = kMbapSize + kMaxPdu;

namespace fc {
constexpr uint8_t kReadCoils = 0x01;
constexpr uint8_t kReadDiscreteInputs = 0x02;
constexpr uint8_t kReadHoldingRegisters = 0x03;
constexpr uint8_t kReadInputRegisters = 0x04;
constexpr uint8_t kWriteSingleCoil = 0x05;
constexpr uint8_t kWriteSingleRegister = 0x06;
constexpr uint8_t kWriteMultipleCoils = 0x0F;
constexpr uint8_t kWriteMultipleRegisters = 0x10;
constexpr uint8_t kMaskWriteRegister = 0x16;
constexpr uint8_t kReadWriteMultipleRegisters = 0x17;
constexpr uint8_t kEncapsulatedInterface = 0x2B;
constexpr uint8_t kMeiReadDeviceId = 0x0E;
} // namespace fc

namespace ex {
constexpr uint8_t kIllegalFunction = 0x01;
constexpr uint8_t kIllegalDataAddress = 0x02;
constexpr uint8_t kIllegalDataValue = 0x03;
constexpr uint8_t kServerDeviceFailure = 0x04;
constexpr uint8_t kAcknowledge = 0x05;
constexpr uint8_t kServerDeviceBusy = 0x06;
constexpr uint8_t kGatewayPathUnavailable = 0x0A;
constexpr uint8_t kGatewayTargetNoResponse = 0x0B;
} // namespace ex

// Quantity limits per request (Modbus Application Protocol v1.1b3).
namespace limits {
constexpr uint16_t kReadBits = 2000;           // FC01 / FC02
constexpr uint16_t kWriteBits = 1968;          // FC15
constexpr uint16_t kReadRegisters = 125;       // FC03 / FC04 / FC23 read part
constexpr uint16_t kWriteRegisters = 123;      // FC16
constexpr uint16_t kReadWriteWriteRegisters = 121; // FC23 write part
} // namespace limits

inline const char* exceptionName(uint8_t code)
{
    switch (code) {
    case ex::kIllegalFunction: return "illegal function";
    case ex::kIllegalDataAddress: return "illegal data address";
    case ex::kIllegalDataValue: return "illegal data value";
    case ex::kServerDeviceFailure: return "server device failure";
    case ex::kAcknowledge: return "acknowledge";
    case ex::kServerDeviceBusy: return "server device busy";
    case ex::kGatewayPathUnavailable: return "gateway path unavailable";
    case ex::kGatewayTargetNoResponse: return "gateway target device failed to respond";
    default: return "unknown exception";
    }
}

// Bit i of a bit table <-> byte i/8, bit i%8 (LSB first), as on the Modbus wire.
inline bool getBit(const uint8_t* bytes, size_t bit) { return (bytes[bit / 8] >> (bit % 8)) & 1; }
inline void setBit(uint8_t* bytes, size_t bit, bool v)
{
    if (v)
        bytes[bit / 8] = uint8_t(bytes[bit / 8] | (1u << (bit % 8)));
    else
        bytes[bit / 8] = uint8_t(bytes[bit / 8] & ~(1u << (bit % 8)));
}

inline std::vector<uint8_t> packBits(const std::vector<bool>& bits)
{
    std::vector<uint8_t> out((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        setBit(out.data(), i, bits[i]);
    return out;
}

inline std::vector<bool> unpackBits(const uint8_t* bytes, size_t count)
{
    std::vector<bool> out(count);
    for (size_t i = 0; i < count; ++i)
        out[i] = getBit(bytes, i);
    return out;
}

} // namespace softmb
