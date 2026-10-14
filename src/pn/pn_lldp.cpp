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
    // IEEE 802.3 MAC/PHY configuration: autoneg supported+enabled, advertised 10/100 HD/FD [verify],
    // operational MAU type.
    tlv(w, 127, orgTlv(lldp::kOuiIeee8023, 1,
                       {0x03, 0x6C, 0x00, uint8_t(info.mauType >> 8), uint8_t(info.mauType)}));
    tlv(w, 0, {}); // End of LLDPDU

    std::vector<uint8_t> f = std::move(w.data());
    padFrame(f);
    return f;
}

} // namespace softpn
