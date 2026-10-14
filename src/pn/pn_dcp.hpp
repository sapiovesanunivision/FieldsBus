// DCP (Discovery and basic Configuration Protocol): codec and IO-Device responder.
//
// DCP runs directly on Ethernet (EtherType 0x8892). The IO-Controller (or Proneta / CODESYS
// "Scan for devices") uses it to find devices (Identify), assign the NameOfStation and the IP
// suite (Set), read properties (Get) and make a device flash its LED (Set Control/Signal).
#pragma once

#include "pn_frame.hpp"
#include "pn_station_store.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace softpn {

// One DCP block. `data` is the raw block payload, i.e. it includes the 2-byte BlockInfo
// (responses) or BlockQualifier (Set requests). In a Get request the blocks are bare
// option/suboption pairs: data is empty and `bare` is true.
struct DcpBlock {
    uint8_t option = 0;
    uint8_t suboption = 0;
    std::vector<uint8_t> data;
    bool bare = false;
};

struct DcpPdu {
    uint16_t frameId = 0;
    uint8_t serviceId = 0;
    uint8_t serviceType = 0;
    uint32_t xid = 0;
    uint16_t responseDelay = 0; // Identify request: ResponseDelayFactor; otherwise reserved
    std::vector<DcpBlock> blocks;
};

// Parses a DCP PDU starting at the FrameID. Throws ParseError on malformed input.
DcpPdu parseDcp(const uint8_t* p, size_t n);
// Serializes from the FrameID on (blocks padded to even length).
std::vector<uint8_t> serializeDcp(const DcpPdu& pdu);
// Complete untagged Ethernet frame (padded to 60 bytes).
std::vector<uint8_t> buildDcpFrame(const MacAddress& dst, const MacAddress& src, const DcpPdu& pdu);

// Block with a 2-byte BlockInfo / BlockQualifier followed by payload.
DcpBlock makeDcpBlock(uint8_t option, uint8_t suboption, uint16_t infoOrQualifier, const void* payload = nullptr,
                      size_t n = 0);
inline DcpBlock makeDcpBlock(uint8_t option, uint8_t suboption, uint16_t infoOrQualifier, const std::string& s)
{
    return makeDcpBlock(option, suboption, infoOrQualifier, s.data(), s.size());
}
// Payload after the 2-byte BlockInfo/BlockQualifier (empty if the block is shorter).
std::vector<uint8_t> dcpPayload(const DcpBlock& b);
uint16_t dcpQualifier(const DcpBlock& b);

// NameOfStation rules (IEC 61158-6-10): DNS-compatible, lowercase. An empty name is valid (= unnamed).
bool isValidStationName(const std::string& name, std::string* why = nullptr);

class DcpResponder {
public:
    struct Config {
        MacAddress mac;
        uint16_t vendorId = 0;
        uint16_t deviceId = 0;
        std::string vendorValue; // DeviceVendorValue / TypeOfStation
    };
    struct Hooks {
        std::function<bool(const std::vector<uint8_t>&)> send;  // required
        std::function<void(const std::string&)> log;
        std::function<void()> onSignal;                        // "flash LED"
        std::function<bool()> inOperation;                     // true: refuse name/IP changes
        std::function<void(const StationData&)> persist;       // permanent Set (name, ipSet)
        std::function<void()> onIdentityChanged;               // name changed (LLDP refresh)
    };

    DcpResponder(Config config, Hooks hooks);

    void setStation(const StationData& data);
    StationData station() const;
    void setIpSuite(uint32_t ip, uint32_t mask, uint32_t gateway);

    // RX thread: handles Identify / Get / Set addressed to us (or to the Identify multicast).
    void onFrame(const EthHeader& eth, const uint8_t* frame, size_t len);
    // Housekeeping thread: sends Identify responses whose ResponseDelay has expired.
    void poll(std::chrono::steady_clock::time_point now);

private:
    using Clock = std::chrono::steady_clock;

    bool identifyMatches(const DcpPdu& req) const;
    std::vector<DcpBlock> identityBlocks() const;
    bool getBlock(uint8_t option, uint8_t suboption, DcpBlock& out) const;
    uint8_t handleSet(const DcpBlock& b, bool& changedPersistent, bool& changedName);
    void log(const std::string& m) const
    {
        if (hooks_.log)
            hooks_.log(m);
    }

    Config cfg_;
    Hooks hooks_;
    mutable std::mutex mutex_;
    StationData station_;
    uint32_t ip_ = 0, mask_ = 0, gateway_ = 0;
    struct Pending {
        Clock::time_point due;
        std::vector<uint8_t> frame;
    };
    std::vector<Pending> pending_;
};

} // namespace softpn
