// softpn_ndis_test - user-mode test tool for softpn_ndis.sys (replaces the sample's prottest).
//
//   softpn_ndis_test list
//   softpn_ndis_test selftest <adapter>                 one-machine checks, prints PASS/FAIL
//   softpn_ndis_test send <adapter> [-n count] [-t type] [-m dst-mac] [--vlan] [-i interval-ms]
//   softpn_ndis_test recv <adapter> [-t type ...] [--vlan] [-s seconds]
//
// <adapter>: "\DEVICE\{GUID}" as shown by list, or its index in the list.
// Defaults: type 0x88B5 (local experimental), destination broadcast, recv types 0x8892 0x88CC 0x88B5.
// Must run elevated (the device is SYSTEM + Administrators only).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <ntddndis.h>

#include "softpn_ndis_ioctl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Binding {
    std::wstring name;   // \DEVICE\{GUID}
    std::wstring descr;
};

HANDLE openControl()
{
    return CreateFileW(SOFTPN_NDIS_DOS_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED, nullptr);
}

// DeviceIoControl on an overlapped handle, waited for synchronously.
bool ioctl(HANDLE h, DWORD code, const void* in, DWORD inLen, void* out, DWORD outLen, DWORD* got = nullptr)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0;
    BOOL ok = DeviceIoControl(h, code, const_cast<void*>(in), inLen, out, outLen, &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(h, &ov, &n, TRUE);
    DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    if (got)
        *got = n;
    SetLastError(err);
    return ok != FALSE;
}

std::vector<Binding> listBindings(HANDLE h)
{
    std::vector<Binding> list;
    // Waits (max 5 s) until the driver has bound to all adapters.
    ioctl(h, IOCTL_NDISPROT_BIND_WAIT, nullptr, 0, nullptr, 0);
    for (ULONG i = 0;; ++i) {
        alignas(8) uint8_t buf[2048] = {};
        auto* q = reinterpret_cast<NDISPROT_QUERY_BINDING*>(buf);
        q->BindingIndex = i;
        DWORD got = 0;
        if (!ioctl(h, IOCTL_NDISPROT_QUERY_BINDING, q, sizeof *q, buf, sizeof buf, &got))
            break;
        Binding b;
        b.name.assign(reinterpret_cast<const wchar_t*>(buf + q->DeviceNameOffset), q->DeviceNameLength / sizeof(wchar_t));
        b.descr.assign(reinterpret_cast<const wchar_t*>(buf + q->DeviceDescrOffset), q->DeviceDescrLength / sizeof(wchar_t));
        while (!b.name.empty() && b.name.back() == L'\0')
            b.name.pop_back();
        while (!b.descr.empty() && b.descr.back() == L'\0')
            b.descr.pop_back();
        list.push_back(b);
    }
    return list;
}

std::wstring widen(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// Opens a new handle bound to the adapter (name or index). Returns INVALID_HANDLE_VALUE on error.
HANDLE openAdapter(const std::wstring& selector, std::wstring* nameOut = nullptr)
{
    HANDLE h = openControl();
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("cannot open %ls: error %lu (driver not started, or not elevated?)\n", SOFTPN_NDIS_DOS_NAME,
                    GetLastError());
        return h;
    }
    std::wstring name = selector;
    wchar_t* end = nullptr;
    unsigned long idx = std::wcstoul(selector.c_str(), &end, 10);
    if (!selector.empty() && end && *end == L'\0') {
        auto list = listBindings(h);
        if (idx >= list.size()) {
            std::printf("adapter index %lu not found (%zu bindings)\n", idx, list.size());
            CloseHandle(h);
            return INVALID_HANDLE_VALUE;
        }
        name = list[idx].name;
    }
    if (!ioctl(h, IOCTL_NDISPROT_OPEN_DEVICE, name.data(), DWORD(name.size() * sizeof(wchar_t)), nullptr, 0)) {
        std::printf("OPEN_DEVICE %ls failed: error %lu\n", name.c_str(), GetLastError());
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    if (nameOut)
        *nameOut = name;
    return h;
}

bool setFilter(HANDLE h, std::vector<uint16_t> types, ULONG flags)
{
    SOFTPN_FILTER f{};
    f.Version = SOFTPN_NDIS_API_VERSION;
    f.Flags = flags;
    f.EtherTypeCount = ULONG(types.size());
    for (size_t i = 0; i < types.size() && i < SOFTPN_MAX_ETHERTYPES; ++i)
        f.EtherTypes[i] = types[i];
    return ioctl(h, IOCTL_SOFTPN_SET_FILTER, &f, sizeof f, nullptr, 0);
}

bool getMac(HANDLE h, uint8_t mac[6])
{
    alignas(8) uint8_t buf[sizeof(NDISPROT_QUERY_OID) + 8] = {};
    auto* q = reinterpret_cast<NDISPROT_QUERY_OID*>(buf);
    q->Oid = OID_802_3_CURRENT_ADDRESS;
    DWORD got = 0;
    if (!ioctl(h, IOCTL_NDISPROT_QUERY_OID_VALUE, buf, sizeof buf, buf, sizeof buf, &got))
        return false;
    std::memcpy(mac, q->Data, 6);
    return true;
}

bool getStats(HANDLE h, SOFTPN_STATS& s)
{
    DWORD got = 0;
    return ioctl(h, IOCTL_SOFTPN_GET_STATS, nullptr, 0, &s, sizeof s, &got) && got == sizeof s;
}

void printStats(HANDLE h)
{
    SOFTPN_STATS s{};
    if (!getStats(h, s)) {
        std::printf("GET_STATS failed: %lu\n", GetLastError());
        return;
    }
    std::printf("stats: rx accepted %llu delivered %llu | dropped queue-full %llu own %llu no-mem %llu | vlan re-inserted %llu\n"
                "       tx %llu errors %llu rejected %llu | queue %lu (high %lu, limit %lu)\n",
                s.RxAccepted, s.RxDelivered, s.RxDroppedQueueFull, s.RxDroppedOwn, s.RxDroppedNoMemory,
                s.RxVlanReinserted, s.TxFrames, s.TxErrors, s.TxRejected, s.QueueDepth, s.QueueHighWater,
                s.QueueLimit);
}

// Builds an Ethernet frame (padded to 60 bytes) with optional 802.1Q tag (priority 6, VID 0).
std::vector<uint8_t> makeFrame(const uint8_t dst[6], const uint8_t src[6], uint16_t type, bool vlan, uint32_t seq)
{
    std::vector<uint8_t> f;
    f.insert(f.end(), dst, dst + 6);
    f.insert(f.end(), src, src + 6);
    if (vlan) {
        f.push_back(0x81);
        f.push_back(0x00);
        f.push_back(0xC0); // priority 6, VID 0 (kVlanTciRt)
        f.push_back(0x00);
    }
    f.push_back(uint8_t(type >> 8));
    f.push_back(uint8_t(type));
    for (int i = 0; i < 4; ++i)
        f.push_back(uint8_t(seq >> (24 - 8 * i)));
    const char tag[] = "softpn_ndis_test";
    f.insert(f.end(), tag, tag + sizeof tag - 1);
    while (f.size() < 60)
        f.push_back(0);
    return f;
}

bool writeFrame(HANDLE h, const std::vector<uint8_t>& f, DWORD* err = nullptr)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0;
    BOOL ok = WriteFile(h, f.data(), DWORD(f.size()), &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(h, &ov, &n, TRUE);
    DWORD e = ok ? 0 : GetLastError();
    CloseHandle(ov.hEvent);
    if (err)
        *err = e;
    return ok && n == f.size();
}

// One read with timeout. Returns bytes read, 0 on timeout (the read is cancelled), -1 on error.
int readFrame(HANDLE h, uint8_t* buf, DWORD size, DWORD timeoutMs)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, size, &n, &ov);
    int result = -1;
    if (ok) {
        result = int(n);
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0) {
            result = GetOverlappedResult(h, &ov, &n, FALSE) ? int(n) : -1;
        } else {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &n, TRUE);
            result = 0;
        }
    }
    CloseHandle(ov.hEvent);
    return result;
}

bool parseMac(const char* s, uint8_t mac[6])
{
    unsigned v[6];
    if (std::sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; ++i)
        mac[i] = uint8_t(v[i]);
    return true;
}

void printFrame(const uint8_t* p, int len)
{
    uint16_t type = uint16_t(p[12] << 8 | p[13]);
    std::printf("%02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x  type %04x", p[6], p[7], p[8],
                p[9], p[10], p[11], p[0], p[1], p[2], p[3], p[4], p[5], type);
    if (type == 0x8100 && len >= 18)
        std::printf(" [vlan prio %d vid %d] inner %04x", p[14] >> 5, (p[14] & 0x0F) << 8 | p[15],
                    uint16_t(p[16] << 8 | p[17]));
    std::printf("  len %d\n", len);
}

// ---- commands ----

int cmdList()
{
    HANDLE h = openControl();
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("cannot open %ls: error %lu (driver not started, or not elevated?)\n", SOFTPN_NDIS_DOS_NAME,
                    GetLastError());
        return 1;
    }
    ULONG ver = 0;
    if (ioctl(h, IOCTL_SOFTPN_GET_VERSION, nullptr, 0, &ver, sizeof ver))
        std::printf("softpn_ndis API version %lu\n", ver);
    auto list = listBindings(h);
    for (size_t i = 0; i < list.size(); ++i)
        std::printf("%2zu. %ls\n     - %ls\n", i, list[i].name.c_str(), list[i].descr.c_str());
    CloseHandle(h);
    return 0;
}

int cmdSend(const char* adapter, int count, uint16_t type, const uint8_t dst[6], bool vlan, int intervalMs)
{
    HANDLE h = openAdapter(widen(adapter));
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    uint8_t src[6];
    if (!getMac(h, src) || !setFilter(h, {type}, SOFTPN_FILTER_VLAN | SOFTPN_FILTER_DROP_OWN)) {
        std::printf("setup failed: %lu\n", GetLastError());
        return 1;
    }
    int ok = 0;
    for (int i = 0; i < count; ++i) {
        DWORD err = 0;
        if (writeFrame(h, makeFrame(dst, src, type, vlan, uint32_t(i)), &err))
            ++ok;
        else
            std::printf("write %d failed: %lu\n", i, err);
        if (intervalMs > 0)
            Sleep(DWORD(intervalMs));
    }
    std::printf("sent %d/%d frames, type %04x%s\n", ok, count, type, vlan ? " (802.1Q prio 6)" : "");
    printStats(h);
    CloseHandle(h);
    return ok == count ? 0 : 1;
}

int cmdRecv(const char* adapter, std::vector<uint16_t> types, bool vlan, int seconds)
{
    HANDLE h = openAdapter(widen(adapter));
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    if (!setFilter(h, types, (vlan ? SOFTPN_FILTER_VLAN : 0) | SOFTPN_FILTER_DROP_OWN)) {
        std::printf("SET_FILTER failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("receiving %d s ...\n", seconds);
    const ULONGLONG end = GetTickCount64() + ULONGLONG(seconds) * 1000;
    int frames = 0;
    uint8_t buf[2048];
    while (GetTickCount64() < end) {
        int n = readFrame(h, buf, sizeof buf, 200);
        if (n < 0) {
            std::printf("read failed: %lu\n", GetLastError());
            break;
        }
        if (n > 0) {
            ++frames;
            if (frames <= 50)
                printFrame(buf, n);
        }
    }
    std::printf("%d frames\n", frames);
    printStats(h);
    CloseHandle(h);
    return 0;
}

int g_fail = 0;
void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_fail;
}

int cmdSelftest(const char* adapter)
{
    std::wstring name;
    HANDLE h = openAdapter(widen(adapter), &name);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    std::printf("selftest on %ls\n", name.c_str());

    ULONG ver = 0;
    check(ioctl(h, IOCTL_SOFTPN_GET_VERSION, nullptr, 0, &ver, sizeof ver) && ver == SOFTPN_NDIS_API_VERSION,
          "GET_VERSION");

    uint8_t mac[6] = {}, bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, other[6] = {0x02, 0, 0, 0, 0, 1};
    check(getMac(h, mac), "QUERY_OID OID_802_3_CURRENT_ADDRESS");

    DWORD err = 0;
    check(!writeFrame(h, makeFrame(bcast, mac, 0x88B5, false, 0), &err), "write refused before SET_FILTER");

    check(!setFilter(h, {0x0100}, 0), "filter with a length (< 0x600) refused");
    check(!setFilter(h, {0x8100}, 0), "filter with 0x8100 refused");
    check(!setFilter(h, {1, 2, 3, 4, 5, 6, 7, 8, 9}, 0), "filter with 9 types refused");
    check(setFilter(h, {0x8892, 0x88CC, 0x88B5}, SOFTPN_FILTER_VLAN | SOFTPN_FILTER_DROP_OWN), "SET_FILTER 8892 88CC 88B5 +vlan");

    check(writeFrame(h, makeFrame(bcast, mac, 0x88B5, false, 1), &err), "write 0x88B5 broadcast");
    check(writeFrame(h, makeFrame(bcast, mac, 0x8892, true, 2), &err), "write 802.1Q prio 6 + 0x8892");
    check(!writeFrame(h, makeFrame(bcast, mac, 0x0800, false, 3), &err), "write 0x0800 refused");
    check(!writeFrame(h, makeFrame(bcast, other, 0x88B5, false, 4), &err), "write with foreign source MAC refused");
    std::vector<uint8_t> runt(10, 0);
    check(!writeFrame(h, runt, &err), "write of 10 bytes refused");

    // Own frames must not come back (no loopback). Send a few, then read with timeout.
    for (uint32_t i = 0; i < 5; ++i)
        writeFrame(h, makeFrame(bcast, mac, 0x88B5, false, 100 + i));
    uint8_t buf[2048];
    int own = 0;
    for (int i = 0; i < 5; ++i) {
        int n = readFrame(h, buf, sizeof buf, 100);
        if (n >= 12 && std::memcmp(buf + 6, mac, 6) == 0)
            ++own;
    }
    check(own == 0, "own frames not received");

    // Cancel of a pending read (used by L2Port::interrupt / close).
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL started = ReadFile(h, buf, sizeof buf, &n, &ov);
        bool pending = !started && GetLastError() == ERROR_IO_PENDING;
        ULONGLONG t0 = GetTickCount64();
        CancelIoEx(h, &ov);
        BOOL done = GetOverlappedResult(h, &ov, &n, TRUE);
        DWORD e = GetLastError();
        ULONGLONG dt = GetTickCount64() - t0;
        CloseHandle(ov.hEvent);
        check(pending && !done && e == ERROR_OPERATION_ABORTED && dt < 500, "CancelIoEx completes a pending read");
    }

    // One handle per adapter.
    {
        HANDLE h2 = openControl();
        bool busy = h2 != INVALID_HANDLE_VALUE &&
                    !ioctl(h2, IOCTL_NDISPROT_OPEN_DEVICE, name.data(), DWORD(name.size() * sizeof(wchar_t)), nullptr, 0);
        check(busy, "second OPEN_DEVICE of the same adapter refused");
        if (h2 != INVALID_HANDLE_VALUE)
            CloseHandle(h2);
    }

    SOFTPN_STATS s{};
    check(getStats(h, s) && s.Version == SOFTPN_NDIS_API_VERSION && s.QueueLimit > 4, "GET_STATS");
    // Rejected by the filter / source check: before SET_FILTER, 0x0800, foreign source (the runt
    // fails the length check before).
    check(s.TxFrames == 7 && s.TxRejected == 3, "tx counters (7 sent, 3 rejected)");
    printStats(h);
    CloseHandle(h);

    // After close, the adapter can be opened again.
    HANDLE h3 = openAdapter(name);
    check(h3 != INVALID_HANDLE_VALUE, "reopen after close");
    if (h3 != INVALID_HANDLE_VALUE)
        CloseHandle(h3);

    std::printf("%s (%d failed)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}

void usage()
{
    std::printf("usage:\n"
                "  softpn_ndis_test list\n"
                "  softpn_ndis_test selftest <adapter>\n"
                "  softpn_ndis_test send <adapter> [-n count] [-t type] [-m dst-mac] [--vlan] [-i interval-ms]\n"
                "  softpn_ndis_test recv <adapter> [-t type ...] [--vlan] [-s seconds]\n"
                "<adapter>: \\DEVICE\\{GUID} or the index shown by list\n");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "list")
        return cmdList();
    if (argc < 3) {
        usage();
        return 2;
    }
    const char* adapter = argv[2];
    int count = 20, seconds = 10, intervalMs = 0;
    bool vlan = false;
    std::vector<uint16_t> types;
    uint8_t dst[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--vlan")
            vlan = true;
        else if (a == "-n" && i + 1 < argc)
            count = std::atoi(argv[++i]);
        else if (a == "-s" && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
        else if (a == "-i" && i + 1 < argc)
            intervalMs = std::atoi(argv[++i]);
        else if (a == "-t" && i + 1 < argc)
            types.push_back(uint16_t(std::strtoul(argv[++i], nullptr, 16)));
        else if (a == "-m" && i + 1 < argc) {
            if (!parseMac(argv[++i], dst)) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (cmd == "selftest")
        return cmdSelftest(adapter);
    if (cmd == "send")
        return cmdSend(adapter, count, types.empty() ? uint16_t(0x88B5) : types[0], dst, vlan, intervalMs);
    if (cmd == "recv") {
        if (types.empty())
            types = {0x8892, 0x88CC, 0x88B5};
        return cmdRecv(adapter, types, vlan, seconds);
    }
    usage();
    return 2;
}
