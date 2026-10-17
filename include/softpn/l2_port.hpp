// Raw Ethernet (Layer 2) access for the PROFINET stack.
//
//   Windows  own NDIS protocol driver softpn_ndis.sys             - src/pn/l2_port_ndis.cpp
//            Npcap (wpcap.dll, delay-loaded from System32\Npcap)  - src/pn/l2_port_npcap.cpp
//            (chosen in src/pn/l2_port_windows.cpp, see L2Backend)
//   Linux    AF_PACKET raw socket (needs CAP_NET_RAW)            - src/pn/l2_port_linux.cpp
//   Tests    in-memory cable between two ports                    - src/pn/l2_port_memory.cpp
//
// The stack only talks to the L2Port interface, so the backends do not touch the protocol code.
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

// Windows raw-Ethernet backend. Auto: the SOFTPN_L2_BACKEND environment variable ("npcap" or
// "ndis") if set, else the softpn_ndis driver if it is installed and running, else Npcap.
// Linux has only AF_PACKET and ignores it.
enum class L2Backend { Auto, Npcap, Ndis };

// "auto", "npcap", "ndis" (case-insensitive). Returns false for anything else.
SOFTPN_API bool parseL2Backend(const std::string& text, L2Backend& out);
SOFTPN_API const char* l2BackendName(L2Backend b);

struct AdapterInfo {
    std::string name;         // backend name: "\Device\NPF_{GUID}" (Npcap), "\DEVICE\{GUID}" (NDIS) or "eth0"
    std::string backend;      // "npcap", "ndis", "af_packet", "memory"
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
    // Npcap cannot join one Ethernet multicast group, so the Npcap port requests the NDIS
    // "all multicast" packet filter instead (Linux joins 01:0E:CF:00:00:00). Without it an Intel
    // I219-LM dropped DCP Identify. Switch promiscuous on only if CODESYS / Proneta still do not
    // find the device. The BPF filter keeps the load low either way.
    // (The NDIS backend ignores it: its driver always requests "all multicast".)
    bool promiscuous = false;
    // Extra EtherType to receive (pn_probe uses the local experimental 0x88B5). 0 = none.
    uint16_t extraEtherType = 0;
    L2Backend backend = L2Backend::Auto;
};

struct LinkStatus {
    uint32_t speedMbps = 0;  // 0 = unknown (no link, virtual adapter, in-memory cable)
    bool fullDuplex = true;
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
    // Current link speed and duplex (queried on each call; LLDP reports it as the MAU type).
    virtual LinkStatus link() const { return {}; }
};

// Lists the adapters of the platform backend. On error returns an empty list and sets *error.
SOFTPN_API std::vector<AdapterInfo> listAdapters(std::string* error = nullptr);
SOFTPN_API std::vector<AdapterInfo> listAdapters(L2Backend backend, std::string* error = nullptr);

// Opens an adapter. `selector` matches the backend name, the friendly name, the MAC address or
// the IPv4 address of the adapter. Returns nullptr and sets *error on failure.
SOFTPN_API std::unique_ptr<L2Port> openL2Port(const std::string& selector, const L2Options& options,
                                   std::string* error = nullptr);

// Two ports joined by a virtual cable (frames sent on one are received on the other).
// For in-process tests without a NIC.
SOFTPN_API std::pair<std::unique_ptr<L2Port>, std::unique_ptr<L2Port>> makeMemoryCable(const AdapterInfo& a,
                                                                            const AdapterInfo& b);

} // namespace softpn
