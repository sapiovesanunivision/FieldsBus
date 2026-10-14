// DCE UUIDs as used by PROFINET (RPC header fields and PNIO blocks such as ARUUID).
//
// Byte order: inside PNIO blocks a UUID is always big-endian. In the RPC header the first three
// fields (Data1, Data2, Data3) follow the packet's data representation (drep), usually
// little-endian; Data4 is always a plain byte array.
#pragma once

#include "pn_defs.hpp"
#include "pn_frame.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace softpn {

struct Uuid {
    uint32_t d1 = 0;
    uint16_t d2 = 0;
    uint16_t d3 = 0;
    std::array<uint8_t, 8> d4{};

    bool operator==(const Uuid& o) const { return d1 == o.d1 && d2 == o.d2 && d3 == o.d3 && d4 == o.d4; }
    bool operator!=(const Uuid& o) const { return !(*this == o); }
    bool operator<(const Uuid& o) const
    {
        if (d1 != o.d1)
            return d1 < o.d1;
        if (d2 != o.d2)
            return d2 < o.d2;
        if (d3 != o.d3)
            return d3 < o.d3;
        return d4 < o.d4;
    }
    bool isNil() const { return *this == Uuid{}; }

    std::string str() const
    {
        char s[37];
        std::snprintf(s, sizeof s, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", d1, d2, d3, d4[0], d4[1],
                      d4[2], d4[3], d4[4], d4[5], d4[6], d4[7]);
        return s;
    }

    // Version-4 (random) UUID, for RPC activity IDs.
    static Uuid random()
    {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        uint64_t a = rng(), b = rng();
        Uuid u;
        u.d1 = uint32_t(a >> 32);
        u.d2 = uint16_t(a >> 16);
        u.d3 = uint16_t((a & 0x0FFF) | 0x4000);
        for (int i = 0; i < 8; ++i)
            u.d4[size_t(i)] = uint8_t(b >> (8 * i));
        u.d4[0] = uint8_t((u.d4[0] & 0x3F) | 0x80);
        return u;
    }
};

inline Uuid readUuid(ByteReader& r, bool littleEndian)
{
    Uuid u;
    if (littleEndian) {
        u.d1 = r.u32();
        u.d2 = r.u16();
        u.d3 = r.u16();
    } else {
        u.d1 = r.u32be();
        u.d2 = r.u16be();
        u.d3 = r.u16be();
    }
    const uint8_t* p = r.take(8);
    for (size_t i = 0; i < 8; ++i)
        u.d4[i] = p[i];
    return u;
}

inline void writeUuid(ByteWriter& w, const Uuid& u, bool littleEndian)
{
    if (littleEndian) {
        w.u32(u.d1);
        w.u16(u.d2);
        w.u16(u.d3);
    } else {
        w.u32be(u.d1);
        w.u16be(u.d2);
        w.u16be(u.d3);
    }
    w.bytes(u.d4.data(), 8);
}

inline Uuid pnioUuid(uint32_t data1)
{
    Uuid u;
    u.d1 = data1;
    u.d2 = kUuidPnioData2;
    u.d3 = kUuidPnioData3;
    for (size_t i = 0; i < 8; ++i)
        u.d4[i] = kUuidPnioData4[i];
    return u;
}

inline Uuid pnioDeviceInterface() { return pnioUuid(kUuidPnioData1Device); }
inline Uuid pnioControllerInterface() { return pnioUuid(kUuidPnioData1Controller); }

// Object UUID of an IO-Device: DEA00000-6C97-11D1-8271-<instance><deviceId><vendorId>.
inline Uuid pnioDeviceObject(uint16_t instance, uint16_t deviceId, uint16_t vendorId)
{
    Uuid u = pnioUuid(kUuidPnioData1Object);
    u.d4[2] = uint8_t(instance >> 8);
    u.d4[3] = uint8_t(instance);
    u.d4[4] = uint8_t(deviceId >> 8);
    u.d4[5] = uint8_t(deviceId);
    u.d4[6] = uint8_t(vendorId >> 8);
    u.d4[7] = uint8_t(vendorId);
    return u;
}

// True for any DEA00000-6C97-11D1-8271-xxxx object UUID (instance/device/vendor not checked).
inline bool isPnioObject(const Uuid& u)
{
    return u.d1 == kUuidPnioData1Object && u.d2 == kUuidPnioData2 && u.d3 == kUuidPnioData3 &&
           u.d4[0] == kUuidPnioData4[0] && u.d4[1] == kUuidPnioData4[1];
}

} // namespace softpn
