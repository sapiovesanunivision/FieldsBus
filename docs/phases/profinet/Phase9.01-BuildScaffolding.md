# Phase 9.01: Build scaffolding, constants and the L2 interface

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Add the `softpn` library to the build (only when the Npcap SDK is available on Windows), the big-endian helpers PROFINET needs, the protocol constants, the `L2Port` interface with its frame helpers and an in-memory "cable" used by the offline tests.

## Documentation References
- [Phase 9 spec](../../profinet-spec.md)
- [Wireshark PROFINET dissectors](https://gitlab.com/wireshark/wireshark/-/tree/master/plugins/epan/profinet)
- [Npcap SDK](https://npcap.com/#download)

---

## Step 9.01.1: Big-endian helpers in the shared byte reader/writer

**File:** `include/softeip/bytes.hpp`

### Purpose
PROFINET blocks are big-endian, CIP is little-endian. The existing reader/writer gets the missing big-endian 32-bit accessors, in-place patching (block lengths are written last) and `pos()`/`seek()`. All additions are new members: existing EtherNet/IP and Modbus code is unchanged.

### Implementation

```cpp
// Little-endian (CIP) / big-endian (sockaddr, PROFINET) serialization helpers.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace softeip {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
    void u16be(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
    void u32be(uint32_t v) { u16be(uint16_t(v >> 16)); u16be(uint16_t(v)); }
    void bytes(const void* p, size_t n)
    {
        auto b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    void bytes(const std::vector<uint8_t>& v) { buf_.insert(buf_.end(), v.begin(), v.end()); }
    void zeros(size_t n) { buf_.insert(buf_.end(), n, uint8_t(0)); }
    void patchU16(size_t offset, uint16_t v)
    {
        buf_[offset] = uint8_t(v);
        buf_[offset + 1] = uint8_t(v >> 8);
    }
    void patchU16be(size_t offset, uint16_t v)
    {
        buf_[offset] = uint8_t(v >> 8);
        buf_[offset + 1] = uint8_t(v);
    }
    void patchU32be(size_t offset, uint32_t v)
    {
        patchU16be(offset, uint16_t(v >> 16));
        patchU16be(offset + 2, uint16_t(v));
    }
    // Overwrites n bytes at offset (the range must already exist).
    void bytesAt(size_t offset, const void* p, size_t n)
    {
        auto b = static_cast<const uint8_t*>(p);
        std::copy(b, b + n, buf_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    size_t size() const { return buf_.size(); }
    std::vector<uint8_t>& data() { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    uint8_t u8()
    {
        need(1);
        return p_[pos_++];
    }
    uint16_t u16()
    {
        uint16_t lo = u8();
        uint16_t hi = u8();
        return uint16_t(lo | (hi << 8));
    }
    uint32_t u32()
    {
        uint32_t lo = u16();
        uint32_t hi = u16();
        return lo | (hi << 16);
    }
    uint16_t u16be()
    {
        uint16_t hi = u8();
        uint16_t lo = u8();
        return uint16_t((hi << 8) | lo);
    }
    uint32_t u32be()
    {
        uint32_t hi = u16be();
        uint32_t lo = u16be();
        return (hi << 16) | lo;
    }
    const uint8_t* take(size_t n)
    {
        need(n);
        const uint8_t* r = p_ + pos_;
        pos_ += n;
        return r;
    }
    void skip(size_t n) { take(n); }
    size_t remaining() const { return n_ - pos_; }
    size_t pos() const { return pos_; }
    void seek(size_t pos)
    {
        if (pos > n_)
            throw ParseError("seek beyond end");
        pos_ = pos;
    }
    const uint8_t* cur() const { return p_ + pos_; }

private:
    void need(size_t n) const
    {
        if (n_ - pos_ < n)
            throw ParseError("truncated packet");
    }

    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

} // namespace softeip
```

### Key Concepts
1. **`patchU16be`**: write a placeholder length, append the block body, then patch the real length.
2. **`ParseError`**: every bounds violation throws; protocol handlers catch it and answer with an error instead of crashing on a malformed frame.

---

## Step 9.01.2: Export macro for the PROFINET classes

**File:** `include/softeip/export.hpp`

### Purpose
Adds `SOFTPN_API` so `softpn::Device` can be exported from `SoftFieldbus.dll` exactly like the EtherNet/IP and Modbus classes (`SOFTFIELDBUS_SHARED` turns all of them on).

### Implementation

```cpp
// Export macros for building the libraries as DLLs / shared libraries.
//
//   static libs (CMake default)  nothing defined           -> SOFTEIP_API etc. are empty
//   softeip alone as a DLL       SOFTEIP_SHARED (+ SOFTEIP_BUILDING_DLL while building it)
//   everything in one DLL        SOFTFIELDBUS_SHARED (+ SOFTFIELDBUS_BUILDING_DLL while building it)
//                                CMake option SOFTFIELDBUS_SHARED, Dev.3 SoftFieldbus*.dll
#pragma once

#if defined(SOFTFIELDBUS_SHARED)
#ifndef SOFTEIP_SHARED
#define SOFTEIP_SHARED
#endif
#ifndef SOFTMB_SHARED
#define SOFTMB_SHARED
#endif
#ifndef SOFTFB_SHARED
#define SOFTFB_SHARED
#endif
#ifndef SOFTPN_SHARED
#define SOFTPN_SHARED
#endif
#endif

#if defined(SOFTFIELDBUS_BUILDING_DLL)
#ifndef SOFTEIP_BUILDING_DLL
#define SOFTEIP_BUILDING_DLL
#endif
#ifndef SOFTMB_BUILDING_DLL
#define SOFTMB_BUILDING_DLL
#endif
#ifndef SOFTFB_BUILDING_DLL
#define SOFTFB_BUILDING_DLL
#endif
#ifndef SOFTPN_BUILDING_DLL
#define SOFTPN_BUILDING_DLL
#endif
#endif

#if defined(_WIN32)
#define SOFTFIELDBUS_DETAIL_EXPORT __declspec(dllexport)
#define SOFTFIELDBUS_DETAIL_IMPORT __declspec(dllimport)
#else
#define SOFTFIELDBUS_DETAIL_EXPORT __attribute__((visibility("default")))
#define SOFTFIELDBUS_DETAIL_IMPORT
#endif

#if !defined(SOFTEIP_API)
#if defined(SOFTEIP_SHARED) && defined(SOFTEIP_BUILDING_DLL)
#define SOFTEIP_API SOFTFIELDBUS_DETAIL_EXPORT
#elif defined(SOFTEIP_SHARED)
#define SOFTEIP_API SOFTFIELDBUS_DETAIL_IMPORT
#else
#define SOFTEIP_API
#endif
#endif

#if !defined(SOFTMB_API)
#if defined(SOFTMB_SHARED) && defined(SOFTMB_BUILDING_DLL)
#define SOFTMB_API SOFTFIELDBUS_DETAIL_EXPORT
#elif defined(SOFTMB_SHARED)
#define SOFTMB_API SOFTFIELDBUS_DETAIL_IMPORT
#else
#define SOFTMB_API
#endif
#endif

#if !defined(SOFTFB_API)
#if defined(SOFTFB_SHARED) && defined(SOFTFB_BUILDING_DLL)
#define SOFTFB_API SOFTFIELDBUS_DETAIL_EXPORT
#elif defined(SOFTFB_SHARED)
#define SOFTFB_API SOFTFIELDBUS_DETAIL_IMPORT
#else
#define SOFTFB_API
#endif
#endif

#if !defined(SOFTPN_API)
#if defined(SOFTPN_SHARED) && defined(SOFTPN_BUILDING_DLL)
#define SOFTPN_API SOFTFIELDBUS_DETAIL_EXPORT
#elif defined(SOFTPN_SHARED)
#define SOFTPN_API SOFTFIELDBUS_DETAIL_IMPORT
#else
#define SOFTPN_API
#endif
#endif
```

### Key Concepts
1. **Same pattern as `SOFTFB_API`**: empty for static libraries, dllexport/dllimport for the DLL build.

---

## Step 9.01.3: `pn_defs.hpp`

**File:** `src/pn/pn_defs.hpp`

### Purpose
One place for every protocol constant: EtherTypes, FrameIDs, DCP options, RPC packet types and opnums, PNIO block types, data status bits, RTA PDU types and the PNIOStatus error codes. Values marked `[verify]` must be confirmed against a CODESYS capture during bring-up (Phase 9.18).

### Implementation

```cpp
// PROFINET IO constants used by the softpn stack (internal header).
//
// Values come from IEC 61158-6-10 as decoded by the Wireshark PROFINET dissectors
// (plugins/epan/profinet/packet-pn-dcp.c, packet-dcerpc-pn-io.c, packet-pn-rt.c).
// Entries marked [verify] still have to be confirmed against a CODESYS capture.
#pragma once

#include <cstddef>
#include <cstdint>

namespace softpn {

// ---- Ethernet ----------------------------------------------------------------------------------
constexpr uint16_t kEtherTypeProfinet = 0x8892;
constexpr uint16_t kEtherTypeLldp = 0x88CC;
constexpr uint16_t kEtherTypeVlan = 0x8100;
constexpr uint16_t kEtherTypeLocalExperimental = 0x88B5; // pn_probe jitter frames, never PROFINET
constexpr uint16_t kVlanTciRt = 0xC000;                  // priority 6, VID 0 (RT and alarm frames)
constexpr size_t kMinFrameSize = 60;                     // without FCS
constexpr size_t kMaxFrameSize = 1518;

// ---- FrameID (first 2 bytes after the EtherType, big-endian) -----------------------------------
constexpr uint16_t kFrameIdRtClass1First = 0x8000;
constexpr uint16_t kFrameIdRtClass1Last = 0xBFFF;
constexpr uint16_t kFrameIdAlarmHigh = 0xFC01;
constexpr uint16_t kFrameIdAlarmLow = 0xFE01;
constexpr uint16_t kFrameIdDcpHello = 0xFEFC;
constexpr uint16_t kFrameIdDcpGetSet = 0xFEFD;
constexpr uint16_t kFrameIdDcpIdentifyReq = 0xFEFE;
constexpr uint16_t kFrameIdDcpIdentifyRes = 0xFEFF;

// ---- DCP ---------------------------------------------------------------------------------------
namespace dcp {
constexpr uint8_t kServiceGet = 3;
constexpr uint8_t kServiceSet = 4;
constexpr uint8_t kServiceIdentify = 5;
constexpr uint8_t kServiceHello = 6;

constexpr uint8_t kTypeRequest = 0x00;
constexpr uint8_t kTypeResponseSuccess = 0x01;
constexpr uint8_t kTypeResponseNotSupported = 0x05; // [verify]

constexpr uint8_t kOptIp = 0x01;
constexpr uint8_t kSubIpMac = 0x01;
constexpr uint8_t kSubIpParameter = 0x02;
constexpr uint8_t kSubIpFullSuite = 0x03;

constexpr uint8_t kOptDeviceProperties = 0x02;
constexpr uint8_t kSubDevVendor = 0x01; // DeviceVendorValue / TypeOfStation
constexpr uint8_t kSubDevNameOfStation = 0x02;
constexpr uint8_t kSubDevId = 0x03;
constexpr uint8_t kSubDevRole = 0x04;
constexpr uint8_t kSubDevOptions = 0x05;
constexpr uint8_t kSubDevAliasName = 0x06;
constexpr uint8_t kSubDevInstance = 0x07;

constexpr uint8_t kOptDhcp = 0x03;

constexpr uint8_t kOptControl = 0x05;
constexpr uint8_t kSubCtrlStart = 0x01;
constexpr uint8_t kSubCtrlStop = 0x02;
constexpr uint8_t kSubCtrlSignal = 0x03;
constexpr uint8_t kSubCtrlResponse = 0x04;
constexpr uint8_t kSubCtrlFactoryReset = 0x05;
constexpr uint8_t kSubCtrlResetToFactory = 0x06;

constexpr uint8_t kOptDeviceInitiative = 0x06;
constexpr uint8_t kOptAllSelector = 0xFF;
constexpr uint8_t kSubAllSelector = 0xFF;

constexpr uint16_t kBlockInfoIpSet = 0x0001;
constexpr uint16_t kQualifierTemporary = 0x0000;
constexpr uint16_t kQualifierPermanent = 0x0001;
constexpr uint16_t kSignalFlashOnce = 0x0100;
constexpr uint8_t kDeviceRoleIoDevice = 0x01;

// BlockError in a Control/Response (5/4) block. [verify]
constexpr uint8_t kErrOk = 0x00;
constexpr uint8_t kErrOptionUnsupported = 0x01;
constexpr uint8_t kErrSuboptionUnsupported = 0x02;
constexpr uint8_t kErrSuboptionNotSet = 0x03;
constexpr uint8_t kErrResource = 0x04;
constexpr uint8_t kErrLocalReasons = 0x05;
constexpr uint8_t kErrInOperation = 0x06;

constexpr uint8_t kMulticastIdentify[6] = {0x01, 0x0E, 0xCF, 0x00, 0x00, 0x00};
constexpr unsigned kResponseDelayUnitMs = 10;
} // namespace dcp

// ---- LLDP --------------------------------------------------------------------------------------
namespace lldp {
constexpr uint8_t kMulticast[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};
constexpr unsigned kTxIntervalMs = 5000;
constexpr uint16_t kTtlSeconds = 20;
constexpr uint8_t kOuiProfinet[3] = {0x00, 0x0E, 0xCF};
constexpr uint8_t kOuiIeee8023[3] = {0x00, 0x12, 0x0F};
constexpr uint16_t kMau100BaseTxFd = 16;
} // namespace lldp

// ---- DCE/RPC (connectionless) ------------------------------------------------------------------
namespace rpc {
constexpr uint16_t kPort = 34964; // 0x8894
constexpr size_t kHeaderSize = 80;
constexpr size_t kMaxFragmentBody = 1392; // 80 + 1392 = 1472 = 1500 - IP - UDP

constexpr uint8_t kPtypeRequest = 0;
constexpr uint8_t kPtypePing = 1;
constexpr uint8_t kPtypeResponse = 2;
constexpr uint8_t kPtypeFault = 3;
constexpr uint8_t kPtypeWorking = 4;
constexpr uint8_t kPtypeNocall = 5;
constexpr uint8_t kPtypeReject = 6;
constexpr uint8_t kPtypeAck = 7;
constexpr uint8_t kPtypeFack = 9;

constexpr uint8_t kFlag1LastFrag = 0x02;
constexpr uint8_t kFlag1Frag = 0x04;
constexpr uint8_t kFlag1NoFack = 0x08;
constexpr uint8_t kFlag1Idempotent = 0x20;

constexpr uint16_t kOpConnect = 0;
constexpr uint16_t kOpRelease = 1;
constexpr uint16_t kOpRead = 2;
constexpr uint16_t kOpWrite = 3;
constexpr uint16_t kOpControl = 4;
constexpr uint16_t kOpReadImplicit = 5;

constexpr uint32_t kNcaUnknownInterface = 0x1C010003; // reject status [verify]
constexpr uint32_t kNcaOpRangeError = 0x1C010002;     // [verify]
} // namespace rpc

// ---- PNIO block types --------------------------------------------------------------------------
namespace block {
constexpr uint16_t kAlarmNotificationHigh = 0x0001;
constexpr uint16_t kAlarmNotificationLow = 0x0002;
constexpr uint16_t kWriteReqHeader = 0x0008;
constexpr uint16_t kReadReqHeader = 0x0009;
constexpr uint16_t kIm0 = 0x0020;
constexpr uint16_t kArBlockReq = 0x0101;
constexpr uint16_t kIocrBlockReq = 0x0102;
constexpr uint16_t kAlarmCrBlockReq = 0x0103;
constexpr uint16_t kExpectedSubmoduleBlockReq = 0x0104;
constexpr uint16_t kPrmEndReq = 0x0110;
constexpr uint16_t kApplicationReadyReq = 0x0112;
constexpr uint16_t kReleaseReq = 0x0114;

constexpr uint16_t kWriteResHeader = 0x8008;
constexpr uint16_t kReadResHeader = 0x8009;
constexpr uint16_t kArBlockRes = 0x8101;
constexpr uint16_t kIocrBlockRes = 0x8102;
constexpr uint16_t kAlarmCrBlockRes = 0x8103;
constexpr uint16_t kModuleDiffBlock = 0x8104;
constexpr uint16_t kArServerBlock = 0x8106;
constexpr uint16_t kPrmEndRes = 0x8110;
constexpr uint16_t kApplicationReadyRes = 0x8112;
constexpr uint16_t kReleaseRes = 0x8114;

constexpr size_t kRecordHeaderLength = 60; // BlockLength of every read/write (res) header
} // namespace block

// ---- AR / IOCR fields --------------------------------------------------------------------------
constexpr uint16_t kArTypeIocarSingle = 0x0001;
constexpr uint32_t kArPropStartupAdvanced = 0x40000000; // ARProperties bit 30
constexpr uint16_t kIocrTypeInput = 0x0001;             // device -> controller
constexpr uint16_t kIocrTypeOutput = 0x0002;            // controller -> device
constexpr uint32_t kIocrPropRtClassMask = 0x0000000F;
constexpr uint16_t kAlarmCrTypeAlarm = 0x0001;
constexpr uint16_t kSendClockFactor1ms = 32;  // 32 x 31.25 us
constexpr uint16_t kMinCsduLength = 40;
constexpr uint16_t kMaxCsduLength = 1440;
constexpr uint32_t kTimeBaseNs = 31250;       // SendClockFactor / CycleCounter unit

constexpr uint16_t kControlPrmEnd = 0x0001;
constexpr uint16_t kControlApplicationReady = 0x0002;
constexpr uint16_t kControlRelease = 0x0004;
constexpr uint16_t kControlDone = 0x0008;

constexpr uint16_t kIndexIm0 = 0xAFF0;
constexpr uint16_t kIndexMultipleWrite = 0xE040;

// ---- Cyclic data -------------------------------------------------------------------------------
constexpr uint8_t kIoxsGood = 0x80;
constexpr uint8_t kIoxsBad = 0x00;
constexpr uint8_t kDataStatusPrimary = 0x01;
constexpr uint8_t kDataStatusValid = 0x04;
constexpr uint8_t kDataStatusRun = 0x10;
constexpr uint8_t kDataStatusStationOk = 0x20;
constexpr uint8_t kDataStatusDefault = kDataStatusPrimary | kDataStatusValid | kDataStatusRun | kDataStatusStationOk; // 0x35

// ---- RTA (alarm transport) ---------------------------------------------------------------------
namespace rta {
constexpr uint8_t kPduData = 0x11; // version 1 << 4 | type
constexpr uint8_t kPduNack = 0x12;
constexpr uint8_t kPduAck = 0x13;
constexpr uint8_t kPduErr = 0x14;
constexpr uint8_t kAddFlagsWindow1 = 0x01;
constexpr uint8_t kAddFlagsTack = 0x10;
constexpr uint16_t kInitialSeq = 0xFFFF; // [verify]
} // namespace rta

// ---- PNIOStatus --------------------------------------------------------------------------------
namespace err {
constexpr uint8_t kCodeConnect = 0xDB;
constexpr uint8_t kCodeRelease = 0xDC;
constexpr uint8_t kCodeControl = 0xDD;
constexpr uint8_t kCodeRead = 0xDE;
constexpr uint8_t kCodeWrite = 0xDF;
constexpr uint8_t kCodeRta = 0xCF;

constexpr uint8_t kDecodePnioRw = 0x80;
constexpr uint8_t kDecodePnio = 0x81;

// ErrorCode1 with kDecodePnio (Connect/Release/Control): faulty block. [verify]
constexpr uint8_t kCode1ArBlock = 0x01;
constexpr uint8_t kCode1IocrBlock = 0x02;
constexpr uint8_t kCode1ExpectedSubmodule = 0x03;
constexpr uint8_t kCode1AlarmCrBlock = 0x04;
constexpr uint8_t kCode1ControlBlock = 0x08;
constexpr uint8_t kCode1ReleaseBlock = 0x09;
constexpr uint8_t kCode1Cmrpc = 0x40;
// ErrorCode2 with kCode1Cmrpc. [verify]
constexpr uint8_t kCmrpcArgsLengthInvalid = 0x00;
constexpr uint8_t kCmrpcUnknownBlocks = 0x01;
constexpr uint8_t kCmrpcIocrMissing = 0x02;
constexpr uint8_t kCmrpcWrongAlarmCrCount = 0x03;
constexpr uint8_t kCmrpcOutOfArResources = 0x04;
constexpr uint8_t kCmrpcArUuidUnknown = 0x05;
constexpr uint8_t kCmrpcStateConflict = 0x06;

// ErrorCode1 with kDecodePnioRw (record access). [verify]
constexpr uint8_t kRwInvalidIndex = 0xB0;
constexpr uint8_t kRwWriteLengthError = 0xB1;
constexpr uint8_t kRwInvalidSlotSubslot = 0xB2;
constexpr uint8_t kRwAccessDenied = 0xB6;

// RTA error (ErrorCode 0xCF, decode 0x81, ErrorCode1 0xFD = RTA_ERR_CLS_PROTOCOL). [verify]
constexpr uint8_t kCode1RtaProtocol = 0xFD;
constexpr uint8_t kRtaAbortConsumerDht = 0x05;
constexpr uint8_t kRtaAbortCmiTimeout = 0x06;
constexpr uint8_t kRtaAbortRequested = 0x0D;
constexpr uint8_t kRtaAbortRerun = 0x0E;
constexpr uint8_t kRtaAbortRelease = 0x0F;
constexpr uint8_t kRtaAbortDeactivated = 0x10;
} // namespace err

// ---- RPC interface/object UUID templates (Data1, Data2, Data3, Data4[8]) -----------------------
constexpr uint32_t kUuidPnioData1Device = 0xDEA00001;
constexpr uint32_t kUuidPnioData1Controller = 0xDEA00002;
constexpr uint32_t kUuidPnioData1Object = 0xDEA00000;
constexpr uint16_t kUuidPnioData2 = 0x6C97;
constexpr uint16_t kUuidPnioData3 = 0x11D1;
constexpr uint8_t kUuidPnioData4[8] = {0x82, 0x71, 0x00, 0xA0, 0x24, 0x42, 0xDF, 0x7D};

} // namespace softpn
```

### Key Concepts
1. **FrameID ranges**: 0x8000-0xBFFF cyclic RT_CLASS_1, 0xFC01/0xFE01 alarms, 0xFEFC-0xFEFF DCP.
2. **DataStatus 0x35**: primary + data valid + provider RUN + station OK, the value a healthy provider sends.
3. **PNIOStatus**: 4 bytes ErrorCode / ErrorDecode / ErrorCode1 / ErrorCode2, e.g. `DB 81 02 0B` = Connect, PNIO, IOCR block, field 11 (reduction ratio).

---

## Step 9.01.4: `l2_port.hpp`

**File:** `include/softpn/l2_port.hpp`

### Purpose
The stack never touches Npcap or sockets directly: it sends and receives Ethernet frames through `L2Port`. Backends: Npcap (Windows), AF_PACKET (Linux), memory (tests). A future NDISPROT-based driver (no Npcap licence) would be one more backend.

### Implementation

```cpp
// Raw Ethernet (Layer 2) access for the PROFINET stack.
//
//   Windows  Npcap (wpcap.dll, delay-loaded from System32\Npcap)  - src/pn/l2_port_npcap.cpp
//   Linux    AF_PACKET raw socket (needs CAP_NET_RAW)            - src/pn/l2_port_linux.cpp
//   Tests    in-memory cable between two ports                    - src/pn/l2_port_memory.cpp
//
// The stack only talks to the L2Port interface, so another backend (for example an
// NDISPROT-based driver instead of Npcap) can be added without touching the protocol code.
#pragma once

#include "softeip/export.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace softpn {

struct SOFTPN_API MacAddress {
    std::array<uint8_t, 6> b{};

    // Accepts "aa:bb:cc:dd:ee:ff" or "aa-bb-cc-dd-ee-ff" (case-insensitive).
    static bool parse(const std::string& text, MacAddress& out);
    static MacAddress fromBytes(const uint8_t* p);
    std::string str() const; // "aa:bb:cc:dd:ee:ff"
    bool isZero() const;
    bool isMulticast() const { return (b[0] & 0x01) != 0; }
    bool operator==(const MacAddress& o) const { return b == o.b; }
    bool operator!=(const MacAddress& o) const { return b != o.b; }
};

struct AdapterInfo {
    std::string name;         // backend name: "\Device\NPF_{GUID}" (Npcap) or "eth0" (Linux)
    std::string friendlyName; // "Ethernet 2" (Windows) / same as name (Linux)
    std::string description;
    MacAddress mac;
    uint32_t ipv4 = 0;        // host byte order, 0 = none
    uint32_t netmask = 0;
    uint32_t gateway = 0;
};

SOFTPN_API std::string ipToString(uint32_t ipHostOrder);
SOFTPN_API bool parseIpv4(const std::string& text, uint32_t& outHostOrder);

struct L2Options {
    // Promiscuous mode. Off by default: on a Hyper-V virtual adapter it made reception stop after a
    // few seconds, and the DCP Identify multicast (01:0E:CF:00:00:00) still arrived without it.
    // Npcap cannot join an Ethernet multicast group, so if a NIC's hardware filter drops that
    // multicast (CODESYS / Proneta do not find the device), switch it on. [verify] on the
    // station NICs. The BPF filter keeps the load low either way.
    bool promiscuous = false;
    // Extra EtherType to receive (pn_probe uses the local experimental 0x88B5). 0 = none.
    uint16_t extraEtherType = 0;
};

class L2Port {
public:
    virtual ~L2Port() = default;
    // Sends one complete Ethernet frame (without FCS). Pads to 60 bytes. Thread safe.
    virtual bool send(const uint8_t* frame, size_t len) = 0;
    bool send(const std::vector<uint8_t>& frame) { return send(frame.data(), frame.size()); }
    // Receives one frame. Returns false on timeout or after interrupt(). Call from one thread only.
    // Frames sent by this port itself are never returned.
    virtual bool receive(std::vector<uint8_t>& frame, int timeoutMs) = 0;
    // Wakes a blocked receive() (used by stop()).
    virtual void interrupt() = 0;
    virtual const AdapterInfo& info() const = 0;
};

// Lists the adapters of the platform backend. On error returns an empty list and sets *error.
SOFTPN_API std::vector<AdapterInfo> listAdapters(std::string* error = nullptr);

// Opens an adapter. `selector` matches the backend name, the friendly name, the MAC address or
// the IPv4 address of the adapter. Returns nullptr and sets *error on failure.
SOFTPN_API std::unique_ptr<L2Port> openL2Port(const std::string& selector, const L2Options& options,
                                   std::string* error = nullptr);

// Two ports joined by a virtual cable (frames sent on one are received on the other).
// For in-process tests without a NIC.
SOFTPN_API std::pair<std::unique_ptr<L2Port>, std::unique_ptr<L2Port>> makeMemoryCable(const AdapterInfo& a,
                                                                            const AdapterInfo& b);

} // namespace softpn
```

### Key Concepts
1. **`receive()` with timeout + `interrupt()`**: lets `stop()` end the RX thread at once.
2. **`promiscuous = false`** by default: on a Hyper-V adapter promiscuous mode stopped reception after a few seconds, and DCP multicast arrived without it (see Phase 9.05 results).
3. **`extraEtherType`**: `pn_probe` also receives its 0x88B5 test frames.

---

## Step 9.01.5: `pn_frame.hpp`

**File:** `src/pn/pn_frame.hpp`

### Purpose
Ethernet II framing shared by DCP, LLDP, cyclic data and alarms: optional 802.1Q tag, padding to 60 bytes, FrameID access.

### Implementation

```cpp
// Ethernet II framing helpers shared by every PROFINET protocol (internal header).
#pragma once

#include "softeip/bytes.hpp"
#include "softpn/l2_port.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace softpn {

using softeip::ByteReader;
using softeip::ByteWriter;
using softeip::ParseError;

struct EthHeader {
    MacAddress dst;
    MacAddress src;
    uint16_t etherType = 0; // after an optional 802.1Q tag
    bool vlan = false;
    uint16_t tci = 0;
    size_t payloadOffset = 0; // first byte after the EtherType
};

// Parses dst/src/[VLAN]/EtherType. Returns false if the frame is too short.
bool parseEthHeader(const uint8_t* frame, size_t len, EthHeader& out);

// Writes dst/src/[0x8100 TCI]/EtherType.
void writeEthHeader(ByteWriter& w, const MacAddress& dst, const MacAddress& src, uint16_t etherType,
                    bool vlan = false, uint16_t tci = 0);

// Pads to the 60-byte Ethernet minimum (FCS is added by the NIC).
void padFrame(std::vector<uint8_t>& frame);

// Reads the PROFINET FrameID at payloadOffset (0 if the payload is shorter than 2 bytes).
uint16_t frameIdOf(const uint8_t* frame, size_t len, const EthHeader& h);

} // namespace softpn
```

### Key Concepts
1. **VLAN tolerance**: received frames may or may not carry a tag (some NIC drivers strip it), so `parseEthHeader` handles both and reports `payloadOffset`.

---

## Step 9.01.6: `pn_frame.cpp`

**File:** `src/pn/pn_frame.cpp`

### Purpose
MAC/IP text conversion (adapter selection by MAC or IP) and the frame helpers.

### Implementation

```cpp
#include "pn_frame.hpp"

#include "pn_defs.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace softpn {

bool MacAddress::parse(const std::string& text, MacAddress& out)
{
    MacAddress m;
    size_t pos = 0;
    for (size_t i = 0; i < 6; ++i) {
        if (pos + 2 > text.size() || !std::isxdigit(static_cast<unsigned char>(text[pos])) ||
            !std::isxdigit(static_cast<unsigned char>(text[pos + 1])))
            return false;
        m.b[i] = static_cast<uint8_t>(std::strtoul(text.substr(pos, 2).c_str(), nullptr, 16));
        pos += 2;
        if (i < 5) {
            if (pos >= text.size() || (text[pos] != ':' && text[pos] != '-'))
                return false;
            ++pos;
        }
    }
    if (pos != text.size())
        return false;
    out = m;
    return true;
}

MacAddress MacAddress::fromBytes(const uint8_t* p)
{
    MacAddress m;
    for (size_t i = 0; i < 6; ++i)
        m.b[i] = p[i];
    return m;
}

std::string MacAddress::str() const
{
    char s[18];
    std::snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return s;
}

bool MacAddress::isZero() const
{
    for (uint8_t v : b)
        if (v != 0)
            return false;
    return true;
}

std::string ipToString(uint32_t ip)
{
    char s[16];
    std::snprintf(s, sizeof s, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return s;
}

bool parseIpv4(const std::string& text, uint32_t& out)
{
    uint32_t ip = 0;
    size_t pos = 0;
    for (int part = 0; part < 4; ++part) {
        size_t start = pos;
        uint32_t v = 0;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) && pos - start < 3)
            v = v * 10 + uint32_t(text[pos++] - '0');
        if (pos == start || v > 255)
            return false;
        ip = (ip << 8) | v;
        if (part < 3) {
            if (pos >= text.size() || text[pos] != '.')
                return false;
            ++pos;
        }
    }
    if (pos != text.size())
        return false;
    out = ip;
    return true;
}

bool parseEthHeader(const uint8_t* frame, size_t len, EthHeader& out)
{
    if (len < 14)
        return false;
    out.dst = MacAddress::fromBytes(frame);
    out.src = MacAddress::fromBytes(frame + 6);
    uint16_t type = uint16_t((frame[12] << 8) | frame[13]);
    size_t off = 14;
    out.vlan = false;
    out.tci = 0;
    if (type == kEtherTypeVlan) {
        if (len < 18)
            return false;
        out.vlan = true;
        out.tci = uint16_t((frame[14] << 8) | frame[15]);
        type = uint16_t((frame[16] << 8) | frame[17]);
        off = 18;
    }
    out.etherType = type;
    out.payloadOffset = off;
    return true;
}

void writeEthHeader(ByteWriter& w, const MacAddress& dst, const MacAddress& src, uint16_t etherType, bool vlan,
                    uint16_t tci)
{
    w.bytes(dst.b.data(), 6);
    w.bytes(src.b.data(), 6);
    if (vlan) {
        w.u16be(kEtherTypeVlan);
        w.u16be(tci);
    }
    w.u16be(etherType);
}

void padFrame(std::vector<uint8_t>& frame)
{
    if (frame.size() < kMinFrameSize)
        frame.resize(kMinFrameSize, 0);
}

uint16_t frameIdOf(const uint8_t* frame, size_t len, const EthHeader& h)
{
    if (len < h.payloadOffset + 2)
        return 0;
    return uint16_t((frame[h.payloadOffset] << 8) | frame[h.payloadOffset + 1]);
}

} // namespace softpn
```

### Key Concepts
1. **Host byte order**: IPv4 addresses are kept as host-order `uint32_t` everywhere in softpn.

---

## Step 9.01.7: `l2_port_memory.cpp`

**File:** `src/pn/l2_port_memory.cpp`

### Purpose
Two `L2Port`s joined by an in-memory cable. `pn_selftest` (Phase 9.15) runs the real device against the controller simulator over it - no NIC, no Npcap, no admin rights.

### Implementation

```cpp
// In-memory "cable" between two L2Ports, for tests without a NIC (pn_selftest).
#include "softpn/l2_port.hpp"

#include "pn_defs.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace softpn {
namespace {

// One direction of the cable.
struct Pipe {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> frames;
    bool interrupted = false;
};

class MemoryPort : public L2Port {
public:
    MemoryPort(AdapterInfo info, std::shared_ptr<Pipe> rx, std::shared_ptr<Pipe> tx)
        : info_(std::move(info)), rx_(std::move(rx)), tx_(std::move(tx))
    {
    }

    bool send(const uint8_t* frame, size_t len) override
    {
        std::vector<uint8_t> f(frame, frame + len);
        if (f.size() < kMinFrameSize)
            f.resize(kMinFrameSize, 0);
        {
            std::lock_guard<std::mutex> lock(tx_->m);
            if (tx_->frames.size() > 4096) // nobody is reading: behave like a full NIC buffer
                return false;
            tx_->frames.push_back(std::move(f));
        }
        tx_->cv.notify_one();
        return true;
    }

    bool receive(std::vector<uint8_t>& frame, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lock(rx_->m);
        rx_->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [this] { return rx_->interrupted || !rx_->frames.empty(); });
        if (rx_->interrupted) {
            rx_->interrupted = false;
            return false;
        }
        if (rx_->frames.empty())
            return false;
        frame = std::move(rx_->frames.front());
        rx_->frames.pop_front();
        return true;
    }

    void interrupt() override
    {
        {
            std::lock_guard<std::mutex> lock(rx_->m);
            rx_->interrupted = true;
        }
        rx_->cv.notify_all();
    }

    const AdapterInfo& info() const override { return info_; }

private:
    AdapterInfo info_;
    std::shared_ptr<Pipe> rx_;
    std::shared_ptr<Pipe> tx_;
};

} // namespace

std::pair<std::unique_ptr<L2Port>, std::unique_ptr<L2Port>> makeMemoryCable(const AdapterInfo& a,
                                                                            const AdapterInfo& b)
{
    auto aToB = std::make_shared<Pipe>();
    auto bToA = std::make_shared<Pipe>();
    return {std::make_unique<MemoryPort>(a, bToA, aToB), std::make_unique<MemoryPort>(b, aToB, bToA)};
}

} // namespace softpn
```

### Key Concepts
1. **Bounded queue**: if nobody reads, `send()` fails like a full NIC buffer instead of growing memory.

---

## Step 9.01.8: Add the softpn library to CMake

**File:** `CMakeLists.txt`

### Purpose
Insert this block **before** the `# ---- Everything in one DLL` section. On Windows the library is built only when `SOFTPN_NPCAP_SDK` (default: the `NPCAP_SDK` environment variable) points to the Npcap SDK, so existing builds and Dev.3 are not affected. Later phases append files to the two lists.

### Implementation

```cmake
# ---- PROFINET RT device (softpn): raw Ethernet via Npcap (Windows) or AF_PACKET (Linux) ----
set(SOFTPN_NPCAP_SDK "$ENV{NPCAP_SDK}" CACHE PATH "Npcap SDK folder (Include/, Lib/x64/); empty = no PROFINET on Windows")
set(SOFTPN_ENABLED OFF)
if(WIN32)
    if(SOFTPN_NPCAP_SDK AND EXISTS "${SOFTPN_NPCAP_SDK}/Include/pcap.h")
        set(SOFTPN_ENABLED ON)
    else()
        message(STATUS "softpn (PROFINET) skipped: set SOFTPN_NPCAP_SDK to the Npcap SDK folder to build it")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SOFTPN_ENABLED ON)
endif()

if(SOFTPN_ENABLED)
    set(SOFTPN_SOURCES
        src/pn/pn_frame.cpp
        src/pn/l2_port_memory.cpp
    )
    set(SOFTPN_HEADERS
        include/softpn/l2_port.hpp
        src/pn/pn_defs.hpp
        src/pn/pn_frame.hpp
    )
    add_library(softpn STATIC ${SOFTPN_SOURCES} ${SOFTPN_HEADERS})
    source_group("Header Files" FILES ${SOFTPN_HEADERS})
    target_include_directories(softpn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/pn)
    target_link_libraries(softpn PUBLIC softeip)
    softeip_warnings(softpn)
    set_target_properties(softpn PROPERTIES FOLDER "native")
endif()
```

### Key Concepts
1. **`SOFTPN_ENABLED`**: one switch that later phases (tools, FieldbusDevice, SoftFieldbus.dll) test.
2. **`target_link_libraries(softpn PUBLIC softeip)`**: reuses the byte helpers, timers and socket layer.

---

## Step 9.01.9: Install the Npcap SDK

### Purpose
Needed to compile the Windows backend (Phase 9.02). The runtime is needed to run it.

### Procedure
1. Download the **Npcap SDK** (e.g. 1.16) from https://npcap.com/#download and unzip it, e.g. to `D:\Software\npcap-sdk-1.16`.
2. Set the user environment variable `NPCAP_SDK` to that folder and restart Visual Studio / VS Code.
3. Install the **Npcap runtime** (free edition, development PCs only - at most 5 systems; shipping on
   customer machines needs Npcap OEM). Installer options: leave "restrict to administrators" unchecked;
   "WinPcap API-compatible mode" is optional (softpn loads `wpcap.dll` from `System32\Npcap` either way).
4. Regenerate: `cmake --preset vs2026` (or `generate_vs2026.bat open`).

---

## Architecture

```
PROFINET protocol code (DCP, RPC, AR, cyclic, RTA)
              |
              v
       +-------------+
       |   L2Port    |  send() / receive(timeout) / interrupt() / info()
       +-------------+
        |     |     |
   Npcap   AF_PACKET  memory cable
  (9.02)    (9.03)    (tests, this phase)
```

---

## Verification Checklist

- [ ] `cmake --preset vs2026` prints no "softpn skipped" message when `NPCAP_SDK` is set, and prints it when it is not
- [ ] The `softpn` project builds with 0 warnings (`/W4`); the rest of the solution is unchanged
- [ ] Linux: `cmake --preset linux && cmake --build --preset linux` builds `softpn` with 0 warnings
- [ ] Existing tests still pass: `eip_scanner_sim` against `eip_adapter_demo`, `mb_client_test` against `mb_server_demo`

---

## Next Phase
Continue to [Phase9.02-L2PortNpcap.md](Phase9.02-L2PortNpcap.md): Npcap backend (Windows).
