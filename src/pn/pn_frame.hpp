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
