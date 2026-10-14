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
