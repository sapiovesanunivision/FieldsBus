// LLDP transmitter (IEEE 802.1AB with the PROFINET TLVs). PROFINET controllers and engineering
// tools use the neighbourhood information for topology; a device must send it every 5 s.
#pragma once

#include "pn_frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace softpn {

struct LldpInfo {
    MacAddress mac;            // interface MAC (also used as ChassisMAC and port MAC on a 1-port PC NIC)
    std::string stationName;   // ChassisID; empty -> the MAC is used
    std::string portId = "port-001";
    uint16_t mauType = 16;     // 100BASE-TX full duplex
};

// Complete LLDP Ethernet frame to 01:80:C2:00:00:0E.
std::vector<uint8_t> buildLldpFrame(const LldpInfo& info);

} // namespace softpn
