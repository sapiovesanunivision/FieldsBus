// PNIO blocks carried in the RPC calls (Connect, Write, Read, Control, Release) and the
// I&M0 record. Every block starts with BlockType(2), BlockLength(2), BlockVersionHigh(1),
// BlockVersionLow(1); BlockLength counts the bytes after the BlockLength field. All block
// fields are big-endian. Both directions are implemented: the device parses requests and
// builds responses, pn_controller_sim does the opposite.
#pragma once

#include "pn_rpc.hpp"
#include "pn_uuid.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace softpn {

// One block inside an NDR argument buffer. `body` starts after the two version bytes.
struct RawBlock {
    uint16_t type = 0;
    uint8_t versionHigh = 0;
    uint8_t versionLow = 0;
    const uint8_t* body = nullptr;
    size_t bodyLength = 0;
};

// Reads the block at p; `consumed` = bytes of the whole block (header included).
// Throws ParseError on a truncated block.
RawBlock readBlock(const uint8_t* p, size_t n, size_t& consumed);
// Splits a buffer that contains only blocks. Throws ParseError on a truncated block.
std::vector<RawBlock> splitBlocks(const uint8_t* p, size_t n);

// Writes the block header on construction and patches BlockLength when finished/destroyed.
class BlockWriter {
public:
    BlockWriter(ByteWriter& w, uint16_t type, uint8_t versionHigh = 1, uint8_t versionLow = 0);
    ~BlockWriter() { finish(); }
    BlockWriter(const BlockWriter&) = delete;
    BlockWriter& operator=(const BlockWriter&) = delete;
    void finish();

private:
    ByteWriter& w_;
    size_t start_;
    bool done_ = false;
};

// ---- Connect -------------------------------------------------------------------------------------

struct ArBlockReq {
    uint16_t arType = 0;
    Uuid arUuid;
    uint16_t sessionKey = 0;
    MacAddress cmInitiatorMac;
    Uuid cmInitiatorObjectUuid;
    uint32_t arProperties = 0;
    uint16_t activityTimeoutFactor = 0; // x 100 ms
    uint16_t cmInitiatorUdpPort = 0;
    std::string cmInitiatorStationName;
};

struct IoDataObject {
    uint16_t slot = 0;
    uint16_t subslot = 0;
    uint16_t frameOffset = 0;
};

struct IocrApi {
    uint32_t api = 0;
    std::vector<IoDataObject> ioData; // data objects (data followed by IOPS)
    std::vector<IoDataObject> iocs;   // consumer status bytes for the other direction
};

struct IocrBlockReq {
    uint16_t iocrType = 0;
    uint16_t iocrReference = 0;
    uint16_t lt = 0;
    uint32_t iocrProperties = 0;
    uint16_t dataLength = 0;
    uint16_t frameId = 0;
    uint16_t sendClockFactor = 0;
    uint16_t reductionRatio = 0;
    uint16_t phase = 0;
    uint16_t sequence = 0;
    uint32_t frameSendOffset = 0;
    uint16_t watchdogFactor = 0;
    uint16_t dataHoldFactor = 0;
    uint16_t tagHeader = 0;
    MacAddress multicastMac;
    std::vector<IocrApi> apis;
};

struct AlarmCrBlockReq {
    uint16_t alarmCrType = 0;
    uint16_t lt = 0;
    uint32_t properties = 0;
    uint16_t rtaTimeoutFactor = 0; // x 100 ms
    uint16_t rtaRetries = 0;
    uint16_t localAlarmReference = 0;
    uint16_t maxAlarmDataLength = 0;
    uint16_t tagHeaderHigh = 0;
    uint16_t tagHeaderLow = 0;
};

struct DataDescription {
    uint16_t type = 0;   // 1 input, 2 output
    uint16_t length = 0; // SubmoduleDataLength
    uint8_t lengthIocs = 1;
    uint8_t lengthIops = 1;
};

struct ExpectedSubmodule {
    uint16_t subslot = 0;
    uint32_t ident = 0;
    uint16_t properties = 0; // bits 0-1: 0 no IO, 1 input, 2 output, 3 input+output
    std::vector<DataDescription> data;
};

struct ExpectedModule {
    uint32_t api = 0;
    uint16_t slot = 0;
    uint32_t moduleIdent = 0;
    uint16_t moduleProperties = 0;
    std::vector<ExpectedSubmodule> submodules;
};

struct ConnectRequest {
    bool hasAr = false;
    ArBlockReq ar;
    std::vector<IocrBlockReq> iocrs;
    std::vector<AlarmCrBlockReq> alarmCrs;
    std::vector<ExpectedModule> expected;
    size_t unknownBlocks = 0;
};

// Parses all Connect blocks. On a malformed block returns the PNIOStatus to send back.
PnioStatus parseConnectRequest(const uint8_t* p, size_t n, ConnectRequest& out);

ArBlockReq parseArBlockReq(const RawBlock& b);
IocrBlockReq parseIocrBlockReq(const RawBlock& b);
AlarmCrBlockReq parseAlarmCrBlockReq(const RawBlock& b);
std::vector<ExpectedModule> parseExpectedSubmoduleBlockReq(const RawBlock& b);

void writeArBlockReq(ByteWriter& w, const ArBlockReq& ar);
void writeIocrBlockReq(ByteWriter& w, const IocrBlockReq& cr);
void writeAlarmCrBlockReq(ByteWriter& w, const AlarmCrBlockReq& a);
void writeExpectedSubmoduleBlockReq(ByteWriter& w, const ExpectedModule& m); // one slot per block

// Connect response blocks.
struct ArBlockRes {
    uint16_t arType = 0;
    Uuid arUuid;
    uint16_t sessionKey = 0;
    MacAddress cmResponderMac;
    uint16_t cmResponderUdpPort = 0;
};
struct IocrBlockRes {
    uint16_t iocrType = 0;
    uint16_t iocrReference = 0;
    uint16_t frameId = 0;
};
struct AlarmCrBlockRes {
    uint16_t alarmCrType = 0;
    uint16_t localAlarmReference = 0;
    uint16_t maxAlarmDataLength = 0;
};

// SubmoduleState (ModuleDiffBlock): FormatIndicator (bit 15) + IdentInfo (bits 11-14).
constexpr uint16_t kSubmoduleStateOk = 0x8000;
constexpr uint16_t kSubmoduleStateSubstitute = 0x8800;
constexpr uint16_t kSubmoduleStateWrong = 0x9000;
constexpr uint16_t kSubmoduleStateNoSubmodule = 0x9800;
// ModuleState
constexpr uint16_t kModuleStateNoModule = 0;
constexpr uint16_t kModuleStateWrong = 1;
constexpr uint16_t kModuleStateProper = 2;
constexpr uint16_t kModuleStateSubstitute = 3;

struct DiffSubmodule {
    uint16_t subslot = 0;
    uint32_t ident = 0;
    uint16_t state = 0;
};
struct DiffModule {
    uint32_t api = 0;
    uint16_t slot = 0;
    uint32_t ident = 0;
    uint16_t state = 0;
    std::vector<DiffSubmodule> submodules;
};

void writeArBlockRes(ByteWriter& w, const ArBlockRes& r);
void writeIocrBlockRes(ByteWriter& w, const IocrBlockRes& r);
void writeAlarmCrBlockRes(ByteWriter& w, const AlarmCrBlockRes& r);
void writeModuleDiffBlock(ByteWriter& w, const std::vector<DiffModule>& diff);
void writeArServerBlock(ByteWriter& w, const std::string& stationName);

ArBlockRes parseArBlockRes(const RawBlock& b);
IocrBlockRes parseIocrBlockRes(const RawBlock& b);
AlarmCrBlockRes parseAlarmCrBlockRes(const RawBlock& b);
std::vector<DiffModule> parseModuleDiffBlock(const RawBlock& b);

// ---- Control (PrmEnd / ApplicationReady / Release) ----------------------------------------------

struct ControlBlock {
    uint16_t type = 0; // 0x0110/0x8110, 0x0112/0x8112, 0x0114/0x8114
    Uuid arUuid;
    uint16_t sessionKey = 0;
    uint16_t command = 0;
    uint16_t properties = 0;
};

ControlBlock parseControlBlock(const RawBlock& b);
void writeControlBlock(ByteWriter& w, const ControlBlock& c);

// ---- Record read / write -------------------------------------------------------------------------

struct RecordHeader {
    uint16_t type = 0; // 0x0008/0x8008 write, 0x0009/0x8009 read
    uint16_t seqNumber = 0;
    Uuid arUuid;
    uint32_t api = 0;
    uint16_t slot = 0;
    uint16_t subslot = 0;
    uint16_t index = 0;
    uint32_t recordDataLength = 0;
    Uuid targetArUuid;            // implicit read only
    uint16_t additionalValue1 = 0; // responses
    uint16_t additionalValue2 = 0;
    PnioStatus status;             // write response only
};

// Parses a read/write header. Record data is NOT part of the block: in a Write request and a
// Read response the RecordDataLength bytes of data follow the header block directly.
RecordHeader parseRecordHeader(const RawBlock& b);
void writeRecordHeader(ByteWriter& w, const RecordHeader& h);

// ---- I&M0 ----------------------------------------------------------------------------------------

struct Im0Data {
    uint16_t vendorId = 0;
    std::string orderId;      // 20 chars, space padded
    std::string serialNumber; // 16 chars, space padded
    uint16_t hardwareRevision = 0;
    char swRevisionPrefix = 'V';
    uint8_t swRevision[3] = {0, 0, 0}; // functional enhancement, bug fix, internal change
    uint16_t revisionCounter = 0;
    uint16_t profileId = 0;
    uint16_t profileSpecificType = 0;
    uint16_t imVersion = 0x0101;
    uint16_t imSupported = 0; // bit mask of I&M1..15 support: none
};

void writeIm0Block(ByteWriter& w, const Im0Data& im0);
Im0Data parseIm0Block(const RawBlock& b);

} // namespace softpn
