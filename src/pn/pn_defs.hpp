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
