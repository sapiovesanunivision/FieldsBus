// EtherNet/IP encapsulation and CIP protocol constants (ODVA CIP Vol. 1 & 2).
#pragma once

#include <cstddef>
#include <cstdint>

namespace softeip {

constexpr uint16_t kEncapPort = 44818; // TCP explicit messaging + UDP ListIdentity
constexpr uint16_t kIoPort = 2222;     // UDP class 0/1 implicit I/O

namespace encap {
constexpr size_t kHeaderSize = 24;

constexpr uint16_t kNop = 0x0000;
constexpr uint16_t kListServices = 0x0004;
constexpr uint16_t kListIdentity = 0x0063;
constexpr uint16_t kListInterfaces = 0x0064;
constexpr uint16_t kRegisterSession = 0x0065;
constexpr uint16_t kUnRegisterSession = 0x0066;
constexpr uint16_t kSendRRData = 0x006F;
constexpr uint16_t kSendUnitData = 0x0070;

constexpr uint32_t kStatusSuccess = 0x0000;
constexpr uint32_t kStatusInvalidCommand = 0x0001;
constexpr uint32_t kStatusIncorrectData = 0x0003;
constexpr uint32_t kStatusInvalidSession = 0x0064;
constexpr uint32_t kStatusInvalidLength = 0x0065;
constexpr uint32_t kStatusUnsupportedProtocol = 0x0069;
} // namespace encap

// Common Packet Format item type IDs.
namespace cpf {
constexpr uint16_t kNullAddress = 0x0000;
constexpr uint16_t kListIdentity = 0x000C;
constexpr uint16_t kConnectedAddress = 0x00A1;
constexpr uint16_t kConnectedData = 0x00B1;
constexpr uint16_t kUnconnectedData = 0x00B2;
constexpr uint16_t kListServices = 0x0100;
constexpr uint16_t kSockaddrO2T = 0x8000;
constexpr uint16_t kSockaddrT2O = 0x8001;
constexpr uint16_t kSequencedAddress = 0x8002;
} // namespace cpf

namespace cip {
// Services
constexpr uint8_t kGetAttributesAll = 0x01;
constexpr uint8_t kReset = 0x05;
constexpr uint8_t kGetAttributeSingle = 0x0E;
constexpr uint8_t kSetAttributeSingle = 0x10;
constexpr uint8_t kForwardClose = 0x4E;
constexpr uint8_t kUnconnectedSend = 0x52;
constexpr uint8_t kForwardOpen = 0x54;
constexpr uint8_t kLargeForwardOpen = 0x5B;
constexpr uint8_t kReplyFlag = 0x80;

// Classes
constexpr uint16_t kIdentityClass = 0x01;
constexpr uint16_t kMessageRouterClass = 0x02;
constexpr uint16_t kAssemblyClass = 0x04;
constexpr uint16_t kConnectionManagerClass = 0x06;

// General status codes
constexpr uint8_t kSuccess = 0x00;
constexpr uint8_t kConnectionFailure = 0x01;
constexpr uint8_t kPathSegmentError = 0x04;
constexpr uint8_t kPathDestinationUnknown = 0x05;
constexpr uint8_t kServiceNotSupported = 0x08;
constexpr uint8_t kAttributeNotSettable = 0x0E;
constexpr uint8_t kNotEnoughData = 0x13;
constexpr uint8_t kAttributeNotSupported = 0x14;
constexpr uint8_t kObjectDoesNotExist = 0x16;

// Connection Manager extended status codes (general status 0x01)
namespace cm {
constexpr uint16_t kDuplicateForwardOpen = 0x0100;
constexpr uint16_t kTransportClassNotSupported = 0x0103;
constexpr uint16_t kOwnershipConflict = 0x0106;
constexpr uint16_t kConnectionNotFound = 0x0107;
constexpr uint16_t kInvalidConnectionSize = 0x0109;
constexpr uint16_t kRpiNotSupported = 0x0111;
constexpr uint16_t kOutOfConnections = 0x0113;
constexpr uint16_t kVendorOrProductMismatch = 0x0114;
constexpr uint16_t kDeviceTypeMismatch = 0x0115;
constexpr uint16_t kRevisionMismatch = 0x0116;
constexpr uint16_t kInvalidApplicationPath = 0x0117;
constexpr uint16_t kNonListenOnlyNotOpened = 0x0119;
constexpr uint16_t kInvalidO2TConnectionType = 0x0123;
constexpr uint16_t kInvalidT2OConnectionType = 0x0124;
constexpr uint16_t kInvalidO2TConnectionSize = 0x0127;
constexpr uint16_t kInvalidT2OConnectionSize = 0x0128;
constexpr uint16_t kInvalidSegmentInPath = 0x0315;
} // namespace cm
} // namespace cip

} // namespace softeip
