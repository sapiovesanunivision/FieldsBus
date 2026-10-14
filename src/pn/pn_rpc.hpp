// DCE/RPC connectionless (CL) protocol over UDP as used by PROFINET IO (port 34964).
//
// PROFINET carries its "context management" (Connect, Write, Control, Release, Read) as RPC
// calls on the PNIO interfaces. This file has the header and NDR codecs and a small UDP
// endpoint that serves requests (device side) and sends requests (ApplicationReady, and the
// controller simulator). Fragmented requests are reassembled, large responses fragmented, and
// the last response per activity is cached so retransmissions get the same answer.
#pragma once

#include "pn_uuid.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace softpn {

struct UdpAddress {
    uint32_t ip = 0; // host byte order
    uint16_t port = 0;
    bool operator==(const UdpAddress& o) const { return ip == o.ip && port == o.port; }
    std::string str() const { return ipToString(ip) + ":" + std::to_string(port); }
};

struct RpcHeader {
    uint8_t ptype = 0;
    uint8_t flags1 = 0;
    uint8_t flags2 = 0;
    bool littleEndian = true; // drep integer representation
    Uuid object;
    Uuid interfaceId;
    Uuid activity;
    uint32_t serverBoot = 0;
    uint32_t interfaceVersion = 1;
    uint32_t seqnum = 0;
    uint16_t opnum = 0;
    uint16_t fragnum = 0;
    uint16_t bodyLength = 0;
};

// Throws ParseError if shorter than 80 bytes or not RPC version 4.
RpcHeader parseRpcHeader(const uint8_t* p, size_t n);
// Writes the 80-byte header (always little-endian, drep 0x10 0x00 0x00).
void writeRpcHeader(ByteWriter& w, const RpcHeader& h, uint16_t bodyLength);

// PNIOStatus: ErrorCode, ErrorDecode, ErrorCode1, ErrorCode2 (always in this byte order).
struct PnioStatus {
    uint8_t code = 0;
    uint8_t decode = 0;
    uint8_t code1 = 0;
    uint8_t code2 = 0;

    bool ok() const { return code == 0 && decode == 0 && code1 == 0 && code2 == 0; }
    std::string str() const
    {
        char s[16];
        std::snprintf(s, sizeof s, "%02X %02X %02X %02X", code, decode, code1, code2);
        return s;
    }
    static PnioStatus make(uint8_t c, uint8_t d, uint8_t c1, uint8_t c2) { return PnioStatus{c, d, c1, c2}; }
};

// NDR arguments of a PNIO call. Request: ArgsMaximum, ArgsLength, array header, blocks.
// Response: PNIOStatus, ArgsLength, array header, blocks.
struct NdrArgs {
    uint32_t argsMaximum = 0; // request: ArgsMaximum; response: MaximumCount
    PnioStatus status;        // response only
    std::vector<uint8_t> blocks;
};

NdrArgs parseNdrRequest(const uint8_t* body, size_t n, bool littleEndian);
NdrArgs parseNdrResponse(const uint8_t* body, size_t n, bool littleEndian);
std::vector<uint8_t> buildNdrRequest(uint32_t argsMaximum, const std::vector<uint8_t>& blocks);
std::vector<uint8_t> buildNdrResponse(const PnioStatus& status, uint32_t argsMaximum, const std::vector<uint8_t>& blocks);

struct RpcRequest {
    RpcHeader header;
    std::vector<uint8_t> body; // reassembled
    UdpAddress from;
};

class RpcEndpoint {
public:
    // Fills `responseBody` and returns true to answer, or returns false to send a "reject"
    // (unknown interface / operation).
    using RequestHandler = std::function<bool(const RpcRequest& request, std::vector<uint8_t>& responseBody)>;
    // A response, fault or reject to one of our own requests.
    using ResponseHandler =
        std::function<void(const RpcHeader& header, const std::vector<uint8_t>& body, const UdpAddress& from)>;

    RpcEndpoint();
    ~RpcEndpoint();
    RpcEndpoint(const RpcEndpoint&) = delete;
    RpcEndpoint& operator=(const RpcEndpoint&) = delete;

    void setRequestHandler(RequestHandler h);
    void setResponseHandler(ResponseHandler h);
    void setServerBoot(uint32_t bootTime);

    bool open(uint16_t port, std::string* error = nullptr, uint32_t bindIp = 0);
    void close();
    bool isOpen() const;
    uint16_t port() const;

    // Waits up to timeoutMs for one datagram and handles it (call in a loop from one thread).
    void poll(int timeoutMs);

    // Sends a request (fragmented if needed). Thread safe with respect to poll().
    bool sendRequest(const UdpAddress& to, const Uuid& object, const Uuid& interfaceId, uint16_t opnum,
                     const Uuid& activity, uint32_t seqnum, const std::vector<uint8_t>& body);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace softpn
