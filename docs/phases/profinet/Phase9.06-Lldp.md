# Phase 9.06: LLDP transmitter

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Send LLDP every 5 s with the PROFINET TLVs, so controllers and engineering tools see the device in their topology view.

## Documentation References
- [IEEE 802.1AB (LLDP)](https://en.wikipedia.org/wiki/Link_Layer_Discovery_Protocol)
- [Wireshark PROFINET dissectors](https://gitlab.com/wireshark/wireshark/-/tree/master/plugins/epan/profinet) (`packet-lldp.c`)

---

## Step 9.06.1: `pn_lldp.hpp`

**File:** `src/pn/pn_lldp.hpp`

### Purpose
One function: build the LLDP frame from the current identity.

### Implementation

```cpp
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
    uint16_t mauType = 16;     // operational MAU type; 16 = 100BASE-TX full duplex (used when the speed is unknown)
};

// Complete LLDP Ethernet frame to 01:80:C2:00:00:0E.
std::vector<uint8_t> buildLldpFrame(const LldpInfo& info);

} // namespace softpn
```

### Key Concepts
1. **Rebuilt on change**: the device rebuilds the frame when the NameOfStation changes (Chassis ID).

---

## Step 9.06.2: `pn_lldp.cpp`

**File:** `src/pn/pn_lldp.cpp`

### Purpose
Chassis ID (NameOfStation, or MAC while unnamed), Port ID `port-001`, TTL 20 s, PROFINET delay / port status / chassis MAC TLVs, IEEE 802.3 MAC/PHY TLV.

### Implementation

```cpp
#include "pn_lldp.hpp"

#include "pn_defs.hpp"

namespace softpn {
namespace {

// TLV header: 7-bit type, 9-bit length.
void tlv(ByteWriter& w, uint8_t type, const std::vector<uint8_t>& value)
{
    w.u16be(uint16_t((unsigned(type & 0x7F) << 9) | unsigned(value.size() & 0x1FF)));
    w.bytes(value);
}

std::vector<uint8_t> orgTlv(const uint8_t oui[3], uint8_t subtype, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> v = {oui[0], oui[1], oui[2], subtype};
    v.insert(v.end(), data.begin(), data.end());
    return v;
}

} // namespace

std::vector<uint8_t> buildLldpFrame(const LldpInfo& info)
{
    ByteWriter w;
    writeEthHeader(w, MacAddress::fromBytes(lldp::kMulticast), info.mac, kEtherTypeLldp);

    // Chassis ID: subtype 7 "locally assigned" = NameOfStation, subtype 4 MAC while unnamed.
    std::vector<uint8_t> chassis;
    if (info.stationName.empty()) {
        chassis.push_back(4);
        chassis.insert(chassis.end(), info.mac.b.begin(), info.mac.b.end());
    } else {
        chassis.push_back(7);
        chassis.insert(chassis.end(), info.stationName.begin(), info.stationName.end());
    }
    tlv(w, 1, chassis);

    // Port ID: subtype 7 "locally assigned". [verify] the exact format CODESYS expects
    std::vector<uint8_t> port = {7};
    port.insert(port.end(), info.portId.begin(), info.portId.end());
    tlv(w, 2, port);

    tlv(w, 3, {uint8_t(lldp::kTtlSeconds >> 8), uint8_t(lldp::kTtlSeconds)}); // TTL

    // PROFINET: LLDP_PNIO_DELAY (all delays unknown = 0).
    tlv(w, 127, orgTlv(lldp::kOuiProfinet, 1, std::vector<uint8_t>(20, 0)));
    // PROFINET: LLDP_PNIO_PORTSTATUS (RTClass2 / RTClass3 port status: off).
    tlv(w, 127, orgTlv(lldp::kOuiProfinet, 2, {0, 0, 0, 0}));
    // PROFINET: LLDP_PNIO_CHASSIS_MAC.
    tlv(w, 127, orgTlv(lldp::kOuiProfinet, 5, std::vector<uint8_t>(info.mac.b.begin(), info.mac.b.end())));
    // IEEE 802.3 MAC/PHY configuration: autoneg supported+enabled, advertised 10/100 HD/FD (and
    // 1000BASE-T FD on a gigabit link), operational MAU type.
    const uint16_t cap = uint16_t(lldp::kAutonegCap10_100 |
                                  (info.mauType == lldp::kMau1000BaseTFd ? lldp::kAutonegCap1000BaseTFd : 0));
    tlv(w, 127, orgTlv(lldp::kOuiIeee8023, 1,
                       {0x03, uint8_t(cap >> 8), uint8_t(cap), uint8_t(info.mauType >> 8), uint8_t(info.mauType)}));
    tlv(w, 0, {}); // End of LLDPDU

    std::vector<uint8_t> f = std::move(w.data());
    padFrame(f);
    return f;
}

} // namespace softpn
```

### Key Concepts
1. **TLV header**: 7-bit type + 9-bit length - built with an unsigned shift (`/analyze` flagged the first version, which shifted a 16-bit value).
2. **Windows LLDP driver**: unbind "Microsoft LLDP Protocol Driver" from the machine NIC, or two LLDP senders share one port.

---

## Step 9.06.3: Add the LLDP files

**File:** `CMakeLists.txt`

### Purpose
Append to the lists:

### Implementation

```cmake
src/pn/pn_lldp.cpp      # SOFTPN_SOURCES
src/pn/pn_lldp.hpp      # SOFTPN_HEADERS
```

### Key Concepts
1. **Transmission** happens in the device's cyclic thread (Phase 9.13).

---

## Verification Checklist

- [ ] `softpn` builds with 0 warnings
- [ ] After Phase 9.13: Wireshark shows an LLDP frame every 5 s from the device with no "malformed" markers
- [ ] `[verify]` with CODESYS: the Port ID format it expects (`port-001`)

---

## Next Phase
Continue to [Phase9.07-RpcCodecAndEndpoint.md](Phase9.07-RpcCodecAndEndpoint.md): DCE/RPC (connectionless) over UDP 34964.
