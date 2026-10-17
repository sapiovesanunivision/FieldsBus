# Phase 9.20: Own NDIS protocol driver backend (no Npcap)

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Redistributing Npcap in a product needs the Npcap OEM licence. Replace it with an own NDIS 6.30 protocol driver (`softpn_ndis.sys`, derived from the Microsoft NDISPROT sample) and a second Windows `L2Port` backend for it, selected at run time with Npcap as fallback. The protocol code does not change. Decision record, driver changes and test results: [HANDOFF-softpn-ndis-driver.md](../../HANDOFF-softpn-ndis-driver.md).

## Documentation References
- [NDISPROT sample](https://github.com/microsoft/Windows-driver-samples/tree/main/network/ndis/ndisprot/6x)
- [NDIS protocol drivers](https://learn.microsoft.com/windows-hardware/drivers/network/ndis-protocol-drivers2)
- [NET_BUFFER_LIST 802.1Q info](https://learn.microsoft.com/windows-hardware/drivers/ddi/ndis/ns-ndis-_ndis_net_buffer_list_8021q_info)
- [Driver signing offerings](https://learn.microsoft.com/windows-hardware/drivers/dashboard/driver-signing-offerings)

---

## Step 9.20.1: The driver (drivers/softpn_ndis)

### Purpose
Built with the WDK (MSBuild), not CMake; this phase only summarises it.

### Procedure
| Folder | Content |
|---|---|
| `upstream/` | the unmodified NDISPROT 6x sample (MS-PL, commit pinned in `ORIGIN.md`): the diff base |
| `sys/` | `softpn_ndis.sys`: changes marked `softpn:` |
| `inc/softpn_ndis_ioctl.h` | the interface shared with user mode (next step) |
| `test/softpn_ndis_test.cpp` | `list`, `selftest`, `send`, `recv` |
| `build.ps1`, `pack_kit.ps1` | build with the WDK NuGet packages (64-bit MSBuild), test kit with install scripts |

Changes against the sample:
- **Naming and access:** device `\\.\SoftPnNdis`, SDDL SYSTEM + Administrators. Service, `Ndi\Service` and
  the NDIS protocol name are all `SoftPnNdis`; INF component `UV_SoftPnNdis`.
- **Per-handle filter** (`IOCTL_SOFTPN_SET_FILTER`): up to 8 EtherTypes, also behind an 802.1Q tag. The handle
  receives and sends nothing until a filter is set, and it may only send with the adapter's source MAC.
- **Receive:** every accepted frame is copied, so the miniport's buffers go back at once. A tag stripped
  by the NIC (OOB `Ieee8021QNetBufferListInfo`) is re-inserted inline. Loopback frames and the own source MAC
  are dropped. The queue holds 256 frames (was 4), with counters (`IOCTL_SOFTPN_GET_STATS`).
- **Send:** no `NDIS_SEND_FLAGS_CHECK_FOR_LOOPBACK`; inline 802.1Q tags accepted.
- **Packet filter:** + ALL_MULTICAST, with a fallback to the multicast list 01:0E:CF:00:00:00 / 01:80:C2:00:00:0E.
- **Fixed sample bug:** a reference leak on the receive path while the adapter is paused.

Install on a test PC (test signing on, Secure Boot off): `install.cmd` from the kit
(`netcfg -l softpn_ndis.inf -c p -i UV_SoftPnNdis`, `net start SoftPnNdis`). A product needs a Microsoft
signature (WHCP or attestation): see the handoff.

### Key Concepts
1. **Protocol driver, not a filter (LWF)**: it binds to every Ethernet adapter like TCP/IP, without sitting in the data path of the other protocols.
2. **One open handle per adapter** (sample design): two programs cannot use the same NIC at the same time.

---

## Step 9.20.2: `softpn_ndis_ioctl.h`

**File:** `drivers/softpn_ndis/inc/softpn_ndis_ioctl.h`

### Purpose
The device names, IOCTL codes and structures of the driver, included by the driver and by `l2_port_ndis.cpp`.

### Implementation

```cpp
/*
 * softpn_ndis_ioctl.h - interface of the softpn NDIS protocol driver (softpn_ndis.sys).
 *
 * Shared by the driver (drivers/softpn_ndis/sys) and the user-mode backend
 * (src/pn/l2_port_ndis.cpp). User mode must include <windows.h> and <ntddndis.h> first.
 *
 * Derived from protuser.h of the Microsoft NDISPROT 6x sample (MS-PL, see
 * drivers/softpn_ndis/upstream/LICENSE-MS-PL.txt). The IOCTL codes 0x200-0x205 are unchanged.
 *
 * Use:
 *   h = CreateFileW(SOFTPN_NDIS_DOS_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
 *                   FILE_FLAG_OVERLAPPED, NULL);
 *   IOCTL_NDISPROT_QUERY_BINDING (index 0, 1, ...)  -> adapter names "\DEVICE\{GUID}"
 *   IOCTL_NDISPROT_OPEN_DEVICE   (UTF-16 adapter name, no terminator) -> one adapter per handle
 *   IOCTL_SOFTPN_SET_FILTER      -> EtherTypes to receive / allowed to send
 *   ReadFile  = one received frame (FCS not included), WriteFile = one frame to send
 */
#ifndef SOFTPN_NDIS_IOCTL_H
#define SOFTPN_NDIS_IOCTL_H

#define SOFTPN_NDIS_NT_NAME   L"\\Device\\SoftPnNdis"
#define SOFTPN_NDIS_LINK_NAME L"\\Global??\\SoftPnNdis"
#define SOFTPN_NDIS_DOS_NAME  L"\\\\.\\SoftPnNdis"

/* Bumped on every incompatible change of the structures below. */
#define SOFTPN_NDIS_API_VERSION 1

#define FSCTL_NDISPROT_BASE FILE_DEVICE_NETWORK

#define _NDISPROT_CTL_CODE(_Function, _Method, _Access) \
    CTL_CODE(FSCTL_NDISPROT_BASE, _Function, _Method, _Access)

#define IOCTL_NDISPROT_OPEN_DEVICE \
    _NDISPROT_CTL_CODE(0x200, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NDISPROT_QUERY_OID_VALUE \
    _NDISPROT_CTL_CODE(0x201, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NDISPROT_SET_OID_VALUE \
    _NDISPROT_CTL_CODE(0x205, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NDISPROT_QUERY_BINDING \
    _NDISPROT_CTL_CODE(0x203, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_NDISPROT_BIND_WAIT \
    _NDISPROT_CTL_CODE(0x204, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/* softpn additions */
#define IOCTL_SOFTPN_GET_VERSION \
    _NDISPROT_CTL_CODE(0x210, METHOD_BUFFERED, FILE_READ_ACCESS)          /* out: ULONG */
#define IOCTL_SOFTPN_SET_FILTER \
    _NDISPROT_CTL_CODE(0x211, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS) /* in: SOFTPN_FILTER */
#define IOCTL_SOFTPN_GET_STATS \
    _NDISPROT_CTL_CODE(0x212, METHOD_BUFFERED, FILE_READ_ACCESS)          /* out: SOFTPN_STATS */

/* Structure to go with IOCTL_NDISPROT_QUERY_OID_VALUE. Data is variable length. */
typedef struct _NDISPROT_QUERY_OID
{
    NDIS_OID            Oid;
    NDIS_PORT_NUMBER    PortNumber;
    UCHAR               Data[sizeof(ULONG)];
} NDISPROT_QUERY_OID, *PNDISPROT_QUERY_OID;

/* Structure to go with IOCTL_NDISPROT_SET_OID_VALUE. Data is variable length. */
typedef struct _NDISPROT_SET_OID
{
    NDIS_OID            Oid;
    NDIS_PORT_NUMBER    PortNumber;
    UCHAR               Data[sizeof(ULONG)];
} NDISPROT_SET_OID, *PNDISPROT_SET_OID;

/*
 * Structure to go with IOCTL_NDISPROT_QUERY_BINDING. In: BindingIndex (0-based). Out: the
 * adapter name ("\DEVICE\{GUID}") and the friendly name, UTF-16, at the given offsets.
 */
typedef struct _NDISPROT_QUERY_BINDING
{
    ULONG            BindingIndex;
    ULONG            DeviceNameOffset;    /* from start of this struct */
    ULONG            DeviceNameLength;    /* in bytes */
    ULONG            DeviceDescrOffset;   /* from start of this struct */
    ULONG            DeviceDescrLength;   /* in bytes */
} NDISPROT_QUERY_BINDING, *PNDISPROT_QUERY_BINDING;

#define SOFTPN_MAX_ETHERTYPES 8

/* Also accept frames tagged 802.1Q (inline 0x8100 or stripped by the NIC) whose inner EtherType
   is in the list. On receive the tag is always delivered inline. */
#define SOFTPN_FILTER_VLAN        0x00000001
/* Drop received frames whose source MAC is the adapter's own address. */
#define SOFTPN_FILTER_DROP_OWN    0x00000002

/*
 * Per open handle. Until the first SET_FILTER (and after a filter with EtherTypeCount 0) nothing
 * is received and nothing can be sent. Sends are allowed only for listed EtherTypes (outer, or
 * inner after an inline 0x8100 tag if SOFTPN_FILTER_VLAN) and only with the adapter's source MAC.
 */
typedef struct _SOFTPN_FILTER
{
    ULONG   Version;                            /* SOFTPN_NDIS_API_VERSION */
    ULONG   Flags;                              /* SOFTPN_FILTER_* */
    ULONG   EtherTypeCount;                     /* 0..SOFTPN_MAX_ETHERTYPES */
    USHORT  EtherTypes[SOFTPN_MAX_ETHERTYPES];  /* host byte order, e.g. 0x8892 */
} SOFTPN_FILTER, *PSOFTPN_FILTER;

/* Counters of one open handle, reset by IOCTL_NDISPROT_OPEN_DEVICE. */
typedef struct _SOFTPN_STATS
{
    ULONG   Version;            /* SOFTPN_NDIS_API_VERSION */
    ULONG   QueueDepth;         /* frames waiting for a read now */
    ULONG   QueueHighWater;     /* maximum of QueueDepth */
    ULONG   QueueLimit;         /* frames kept before the oldest is dropped */
    ULONG64 RxAccepted;         /* frames matching the filter */
    ULONG64 RxDelivered;        /* frames copied to a read */
    ULONG64 RxDroppedQueueFull; /* oldest frames dropped because nobody read */
    ULONG64 RxDroppedOwn;       /* loopback or own source MAC */
    ULONG64 RxDroppedNoMemory;
    ULONG64 RxVlanReinserted;   /* tag stripped by the NIC, put back inline */
    ULONG64 TxFrames;           /* completed sends */
    ULONG64 TxErrors;           /* sends completed with an error */
    ULONG64 TxRejected;         /* writes refused by the EtherType / source MAC check */
} SOFTPN_STATS, *PSOFTPN_STATS;

#endif /* SOFTPN_NDIS_IOCTL_H */
```

### Key Concepts
1. **`SOFTPN_NDIS_API_VERSION`**: the backend checks it with `IOCTL_SOFTPN_GET_VERSION` and refuses a driver that does not match.
2. **Sample IOCTLs unchanged** (0x200-0x205): enumeration (`QUERY_BINDING`), `OPEN_DEVICE`, OID query/set.

---

## Step 9.20.3: `l2_port_windows.hpp`

**File:** `src/pn/l2_port_windows.hpp`

### Purpose
Internal header of the two Windows backends: the shared helpers and the entry points the dispatcher calls.

### Implementation

```cpp
// Internal: shared code of the Windows L2Port backends (Npcap, softpn_ndis driver) and the
// entry points the dispatcher in l2_port_windows.cpp calls.
#pragma once

#include "softpn/l2_port.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <memory>
#include <string>
#include <vector>

namespace softpn {
namespace win {

std::string narrow(const wchar_t* w);
std::string lower(std::string s);

// Adds friendly name, MAC, IPv4/mask/gateway from GetAdaptersAddresses (matched by the GUID in
// AdapterInfo::name), then drops adapters without a MAC (WAN miniports, Bluetooth PAN...).
void addIpHelperInfo(std::vector<AdapterInfo>& list);

// "...{9F36A46A-F0ED-451E-AA72-B88E86EE6191}" -> interface LUID (for GetIfEntry2).
bool luidFromGuidName(const std::string& name, NET_LUID& luid);
LinkStatus linkStatus(const NET_LUID& luid);

// Name, friendly name (case-insensitive), MAC or IPv4. nullptr if none matches.
const AdapterInfo* selectAdapter(const std::vector<AdapterInfo>& list, const std::string& selector);

} // namespace win

#if SOFTPN_HAS_NPCAP
std::vector<AdapterInfo> npcapListAdapters(std::string* error);
std::unique_ptr<L2Port> npcapOpen(const AdapterInfo& adapter, const L2Options& options, std::string* error);
#endif

// softpn_ndis.sys: installed and running (its control device opens)?
bool ndisAvailable(std::string* why = nullptr);
std::vector<AdapterInfo> ndisListAdapters(std::string* error);
std::unique_ptr<L2Port> ndisOpen(const AdapterInfo& adapter, const L2Options& options, std::string* error);

} // namespace softpn
```

### Key Concepts
1. **`SOFTPN_HAS_NPCAP`**: the Npcap functions exist only when the Npcap SDK was found at configure time.

---

## Step 9.20.4: `l2_port_windows.cpp`

**File:** `src/pn/l2_port_windows.cpp`

### Purpose
The public `listAdapters()` / `openL2Port()` on Windows: picks the backend, then shares the IP Helper data, the link state and the adapter selection between both.

### Implementation

```cpp
// Windows L2Port: picks the backend (softpn_ndis driver or Npcap) and holds the code both share
// (IP Helper adapter data, link state, adapter selection).
#include "l2_port_windows.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace softpn {
namespace win {

std::string narrow(const wchar_t* w)
{
    if (!w || !*w)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

void addIpHelperInfo(std::vector<AdapterInfo>& list)
{
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc == NO_ERROR) {
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
            std::string guid = lower(a->AdapterName ? a->AdapterName : "");
            for (AdapterInfo& info : list) {
                if (guid.empty() || lower(info.name).find(guid) == std::string::npos)
                    continue;
                info.friendlyName = narrow(a->FriendlyName);
                if (a->PhysicalAddressLength == 6)
                    info.mac = MacAddress::fromBytes(a->PhysicalAddress);
                for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                    if (u->Address.lpSockaddr->sa_family != AF_INET)
                        continue;
                    auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                    info.ipv4 = ntohl(sin->sin_addr.s_addr);
                    ULONG prefix = u->OnLinkPrefixLength;
                    info.netmask = prefix == 0 ? 0 : (prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1));
                    break;
                }
                for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
                    if (g->Address.lpSockaddr->sa_family != AF_INET)
                        continue;
                    info.gateway = ntohl(reinterpret_cast<sockaddr_in*>(g->Address.lpSockaddr)->sin_addr.s_addr);
                    break;
                }
            }
        }
    }
    // Adapters without a MAC (WAN miniports, Bluetooth PAN...) cannot carry PROFINET.
    list.erase(std::remove_if(list.begin(), list.end(), [](const AdapterInfo& a) { return a.mac.isZero(); }),
               list.end());
}

bool luidFromGuidName(const std::string& name, NET_LUID& luid)
{
    const size_t open = name.find('{');
    if (open == std::string::npos)
        return false;
    unsigned long d1 = 0;
    unsigned d2 = 0, d3 = 0, b[8] = {};
    if (sscanf_s(name.c_str() + open, "{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1],
                 &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    GUID g{};
    g.Data1 = d1;
    g.Data2 = static_cast<unsigned short>(d2);
    g.Data3 = static_cast<unsigned short>(d3);
    for (int i = 0; i < 8; ++i)
        g.Data4[i] = static_cast<unsigned char>(b[i]);
    return ConvertInterfaceGuidToLuid(&g, &luid) == NO_ERROR;
}

LinkStatus linkStatus(const NET_LUID& luid)
{
    LinkStatus ls;
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = luid;
    if (GetIfEntry2(&row) != NO_ERROR || row.MediaConnectState != MediaConnectStateConnected)
        return ls;
    ls.speedMbps = uint32_t(row.TransmitLinkSpeed / 1000000u);
    ls.fullDuplex = true; // MIB_IF_ROW2 has no duplex; autonegotiated 100/1000 links are full duplex
    return ls;
}

const AdapterInfo* selectAdapter(const std::vector<AdapterInfo>& list, const std::string& selector)
{
    MacAddress mac;
    uint32_t ip = 0;
    const bool byMac = MacAddress::parse(selector, mac);
    const bool byIp = parseIpv4(selector, ip);
    for (const AdapterInfo& a : list) {
        if (lower(a.name) == lower(selector) || lower(a.friendlyName) == lower(selector) || (byMac && a.mac == mac) ||
            (byIp && a.ipv4 == ip))
            return &a;
    }
    return nullptr;
}

} // namespace win

namespace {

// Auto -> SOFTPN_L2_BACKEND, else the driver if it runs, else Npcap (if built in).
L2Backend resolve(L2Backend requested)
{
    if (requested != L2Backend::Auto)
        return requested;
    char* env = nullptr;
    size_t len = 0;
    L2Backend fromEnv = L2Backend::Auto;
    if (_dupenv_s(&env, &len, "SOFTPN_L2_BACKEND") == 0 && env) {
        (void)parseL2Backend(env, fromEnv);
        std::free(env);
    }
    if (fromEnv != L2Backend::Auto)
        return fromEnv;
#if SOFTPN_HAS_NPCAP
    return ndisAvailable() ? L2Backend::Ndis : L2Backend::Npcap;
#else
    return L2Backend::Ndis;
#endif
}

#if !SOFTPN_HAS_NPCAP
const char* kNoNpcap = "this build has no Npcap backend (built without SOFTPN_NPCAP_SDK)";
#endif

} // namespace

std::vector<AdapterInfo> listAdapters(L2Backend backend, std::string* error)
{
    if (resolve(backend) == L2Backend::Ndis)
        return ndisListAdapters(error);
#if SOFTPN_HAS_NPCAP
    return npcapListAdapters(error);
#else
    if (error)
        *error = kNoNpcap;
    return {};
#endif
}

std::vector<AdapterInfo> listAdapters(std::string* error)
{
    return listAdapters(L2Backend::Auto, error);
}

std::unique_ptr<L2Port> openL2Port(const std::string& selector, const L2Options& options, std::string* error)
{
    const L2Backend backend = resolve(options.backend);
    std::vector<AdapterInfo> list = listAdapters(backend, error);
    if (list.empty()) {
        if (error && error->empty())
            *error = "no Ethernet adapter found";
        return nullptr;
    }
    const AdapterInfo* found = win::selectAdapter(list, selector);
    if (!found) {
        if (error)
            *error = "adapter '" + selector + "' not found (pn_probe --list shows the names)";
        return nullptr;
    }
    if (backend == L2Backend::Ndis)
        return ndisOpen(*found, options, error);
#if SOFTPN_HAS_NPCAP
    return npcapOpen(*found, options, error);
#else
    if (error)
        *error = kNoNpcap;
    return nullptr;
#endif
}

} // namespace softpn
```

### Key Concepts
1. **`L2Backend::Auto`**: `SOFTPN_L2_BACKEND` (`npcap` / `ndis`) if set, else the driver if `\\.\SoftPnNdis` opens, else Npcap. A PC with the driver needs no Npcap; a PC with only Npcap works as before.
2. **Same adapter names for both**: the IP Helper data is matched by the GUID in the name (`\Device\NPF_{GUID}` or `\DEVICE\{GUID}`), so friendly name, MAC and IPv4 select the NIC either way.

---

## Step 9.20.5: `l2_port_ndis.cpp`

**File:** `src/pn/l2_port_ndis.cpp`

### Purpose
The `L2Port` on the driver: enumeration, open with the PROFINET filter, receive with reads posted in advance, thread-safe send.

### Implementation

```cpp
// softpn_ndis backend of L2Port (Windows): the own NDIS protocol driver softpn_ndis.sys
// (drivers/softpn_ndis), chosen by l2_port_windows.cpp. Replaces Npcap without a redistribution
// licence.
//
// - One overlapped handle per port: the driver allows one open handle per adapter.
// - kPendingReads reads stay posted in the driver. The driver completes them in the order they
//   were posted, so receive() always waits for the oldest one and reposts it after copying.
// - The driver filters (EtherTypes 0x8892, 0x88CC, the optional extra type, 802.1Q-tagged
//   0x8892), never returns our own frames and re-inserts a VLAN tag the NIC stripped.
#include "l2_port_windows.hpp"

#include "pn_defs.hpp"

#include <winioctl.h>
#include <ntddndis.h>

#include "softpn_ndis_ioctl.h"

#include <cstring>
#include <mutex>

namespace softpn {
namespace {

constexpr size_t kPendingReads = 4;
constexpr DWORD kSendTimeoutMs = 1000;

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

HANDLE openControl(bool overlapped)
{
    return CreateFileW(SOFTPN_NDIS_DOS_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                       overlapped ? FILE_FLAG_OVERLAPPED : 0, nullptr);
}

std::string controlError(DWORD err)
{
    if (err == ERROR_FILE_NOT_FOUND)
        return "softpn_ndis driver not running (install it, then: net start SoftPnNdis)";
    if (err == ERROR_ACCESS_DENIED)
        return "softpn_ndis: access denied (the driver allows administrators and services only)";
    return "softpn_ndis: cannot open " + win::narrow(SOFTPN_NDIS_DOS_NAME) + ", error " + std::to_string(err);
}

// DeviceIoControl on an overlapped handle, waited for synchronously.
bool ioctl(HANDLE h, DWORD code, const void* in, DWORD inLen, void* out, DWORD outLen, DWORD* got = nullptr)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return false;
    DWORD n = 0;
    BOOL ok = DeviceIoControl(h, code, const_cast<void*>(in), inLen, out, outLen, &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(h, &ov, &n, TRUE);
    const DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    if (got)
        *got = n;
    SetLastError(err);
    return ok != FALSE;
}

class NdisPort : public L2Port {
public:
    NdisPort(AdapterInfo info, HANDLE h) : info_(std::move(info)), h_(h)
    {
        interruptEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        txEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (Slot& s : slots_) {
            s.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            s.buf.resize(kMaxFrameSize + 4);
        }
        haveLuid_ = win::luidFromGuidName(info_.name, luid_);
        for (Slot& s : slots_)
            post(s);
    }

    ~NdisPort() override
    {
        CancelIoEx(h_, nullptr);
        for (Slot& s : slots_) {
            DWORD n = 0;
            if (s.pending)
                GetOverlappedResult(h_, &s.ov, &n, TRUE);
            if (s.ov.hEvent)
                CloseHandle(s.ov.hEvent);
        }
        CloseHandle(h_);
        if (interruptEvent_)
            CloseHandle(interruptEvent_);
        if (txEvent_)
            CloseHandle(txEvent_);
    }

    bool send(const uint8_t* frame, size_t len) override
    {
        uint8_t padded[kMinFrameSize] = {};
        if (len < kMinFrameSize) {
            std::memcpy(padded, frame, len);
            frame = padded;
            len = kMinFrameSize;
        }
        std::lock_guard<std::mutex> lock(txMutex_);
        OVERLAPPED ov{};
        ov.hEvent = txEvent_;
        ResetEvent(txEvent_);
        DWORD n = 0;
        BOOL ok = WriteFile(h_, frame, DWORD(len), &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(txEvent_, kSendTimeoutMs) != WAIT_OBJECT_0)
                CancelIoEx(h_, &ov); // NIC stuck: give up on this frame
            ok = GetOverlappedResult(h_, &ov, &n, TRUE);
        }
        return ok && n == len;
    }

    bool receive(std::vector<uint8_t>& frame, int timeoutMs) override
    {
        Slot& s = slots_[next_];
        if (!s.pending && !post(s)) {
            Sleep(100); // adapter gone (unbind, driver stopped): don't spin, the caller retries
            return false;
        }
        HANDLE events[2] = {s.ov.hEvent, interruptEvent_};
        DWORD w = WaitForMultipleObjects(2, events, FALSE, DWORD(timeoutMs < 0 ? 0 : timeoutMs));
        if (w != WAIT_OBJECT_0)
            return false; // timeout, interrupt() or wait error
        DWORD n = 0;
        const BOOL ok = GetOverlappedResult(h_, &s.ov, &n, FALSE);
        s.pending = false;
        next_ = (next_ + 1) % kPendingReads;
        if (ok)
            frame.assign(s.buf.data(), s.buf.data() + n);
        else if (GetLastError() != ERROR_OPERATION_ABORTED)
            Sleep(100); // read failed (adapter unbound): don't spin, the caller retries
        post(s);
        return ok && n > 0;
    }

    void interrupt() override { SetEvent(interruptEvent_); }

    const AdapterInfo& info() const override { return info_; }

    LinkStatus link() const override { return haveLuid_ ? win::linkStatus(luid_) : LinkStatus{}; }

private:
    struct Slot {
        OVERLAPPED ov{};
        std::vector<uint8_t> buf;
        bool pending = false;
    };

    // Posts one read. A read that completes at once still signals its event, so receive() treats
    // both cases alike.
    bool post(Slot& s)
    {
        HANDLE ev = s.ov.hEvent;
        s.ov = OVERLAPPED{};
        s.ov.hEvent = ev;
        ResetEvent(ev);
        DWORD n = 0;
        BOOL ok = ReadFile(h_, s.buf.data(), DWORD(s.buf.size()), &n, &s.ov);
        s.pending = ok || GetLastError() == ERROR_IO_PENDING;
        return s.pending;
    }

    AdapterInfo info_;
    HANDLE h_;
    NET_LUID luid_{};
    bool haveLuid_ = false;
    Slot slots_[kPendingReads];
    size_t next_ = 0;
    HANDLE interruptEvent_ = nullptr;
    HANDLE txEvent_ = nullptr;
    std::mutex txMutex_;
};

} // namespace

bool ndisAvailable(std::string* why)
{
    HANDLE h = openControl(false);
    if (h == INVALID_HANDLE_VALUE) {
        if (why)
            *why = controlError(GetLastError());
        return false;
    }
    CloseHandle(h);
    return true;
}

std::vector<AdapterInfo> ndisListAdapters(std::string* error)
{
    std::vector<AdapterInfo> list;
    HANDLE h = openControl(true);
    if (h == INVALID_HANDLE_VALUE) {
        if (error)
            *error = controlError(GetLastError());
        return list;
    }
    // Returns once the driver has bound to all adapters (or after 5 s).
    (void)ioctl(h, IOCTL_NDISPROT_BIND_WAIT, nullptr, 0, nullptr, 0);
    for (ULONG i = 0;; ++i) {
        alignas(8) uint8_t buf[2048] = {};
        auto* q = reinterpret_cast<NDISPROT_QUERY_BINDING*>(buf);
        q->BindingIndex = i;
        DWORD got = 0;
        if (!ioctl(h, IOCTL_NDISPROT_QUERY_BINDING, q, sizeof *q, buf, sizeof buf, &got))
            break;
        if (q->DeviceNameOffset + q->DeviceNameLength > got || q->DeviceDescrOffset + q->DeviceDescrLength > got)
            continue;
        std::wstring name(reinterpret_cast<const wchar_t*>(buf + q->DeviceNameOffset), q->DeviceNameLength / sizeof(wchar_t));
        std::wstring descr(reinterpret_cast<const wchar_t*>(buf + q->DeviceDescrOffset),
                           q->DeviceDescrLength / sizeof(wchar_t));
        AdapterInfo info;
        info.name = win::narrow(name.c_str());
        info.description = win::narrow(descr.c_str());
        info.backend = "ndis";
        list.push_back(info);
    }
    CloseHandle(h);
    win::addIpHelperInfo(list);
    if (list.empty() && error && error->empty())
        *error = "softpn_ndis: no adapter bound";
    return list;
}

std::unique_ptr<L2Port> ndisOpen(const AdapterInfo& adapter, const L2Options& options, std::string* error)
{
    auto fail = [&](const std::string& m) -> std::unique_ptr<L2Port> {
        if (error)
            *error = m;
        return nullptr;
    };
    HANDLE h = openControl(true);
    if (h == INVALID_HANDLE_VALUE)
        return fail(controlError(GetLastError()));

    ULONG version = 0;
    if (!ioctl(h, IOCTL_SOFTPN_GET_VERSION, nullptr, 0, &version, sizeof version) || version != SOFTPN_NDIS_API_VERSION) {
        CloseHandle(h);
        return fail("softpn_ndis: driver API version " + std::to_string(version) + ", expected " +
                    std::to_string(SOFTPN_NDIS_API_VERSION) + " (driver and program do not match)");
    }

    const std::wstring name = widen(adapter.name);
    if (!ioctl(h, IOCTL_NDISPROT_OPEN_DEVICE, name.data(), DWORD(name.size() * sizeof(wchar_t)), nullptr, 0)) {
        const DWORD err = GetLastError();
        CloseHandle(h);
        if (err == ERROR_BUSY)
            return fail("softpn_ndis: " + adapter.friendlyName + " is already opened by another program "
                        "(one program per adapter)");
        return fail("softpn_ndis: cannot open " + adapter.friendlyName + ", error " + std::to_string(err));
    }

    SOFTPN_FILTER f{};
    f.Version = SOFTPN_NDIS_API_VERSION;
    f.Flags = SOFTPN_FILTER_VLAN | SOFTPN_FILTER_DROP_OWN;
    f.EtherTypes[f.EtherTypeCount++] = kEtherTypeProfinet;
    f.EtherTypes[f.EtherTypeCount++] = kEtherTypeLldp;
    if (options.extraEtherType != 0)
        f.EtherTypes[f.EtherTypeCount++] = options.extraEtherType;
    if (!ioctl(h, IOCTL_SOFTPN_SET_FILTER, &f, sizeof f, nullptr, 0)) {
        const DWORD err = GetLastError();
        CloseHandle(h);
        return fail("softpn_ndis: set filter failed, error " + std::to_string(err));
    }
    // options.promiscuous is not needed: the driver requests "all multicast" for every open.
    return std::make_unique<NdisPort>(adapter, h);
}

} // namespace softpn
```

### Key Concepts
1. **Reads posted in advance**: 4 overlapped reads wait in the driver. The driver completes them in posting order, so `receive()` waits for the oldest one only and reposts it after copying.
2. **`interrupt()`**: an event next to the read event, as in the Npcap backend; the destructor cancels the reads (`CancelIoEx`) and waits for them.
3. **Send timeout**: a write that the NIC does not complete within 1 s is cancelled, so a stuck NIC cannot block the cyclic thread forever.

---

## Step 9.20.6: Build both Windows backends; Npcap optional

**File:** `CMakeLists.txt`

### Purpose
On Windows softpn is always built now. The Npcap backend and the `wpcap.dll` delay-load are added only with the SDK. One function applies the settings to `softpn` and `softfieldbus_shared`.

### Implementation

```cmake
set(SOFTPN_HAS_NPCAP OFF)
if(WIN32)
    set(SOFTPN_ENABLED ON)
    if(SOFTPN_NPCAP_SDK AND EXISTS "${SOFTPN_NPCAP_SDK}/Include/pcap.h")
        set(SOFTPN_HAS_NPCAP ON)
    endif()
endif()

function(softpn_windows_l2 target scope)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/drivers/softpn_ndis/inc)
    target_link_libraries(${target} ${scope} iphlpapi)
    if(SOFTPN_HAS_NPCAP)
        target_compile_definitions(${target} PRIVATE SOFTPN_HAS_NPCAP=1)
        # ... Npcap include, wpcap.lib, delayimp, /DELAYLOAD:wpcap.dll as before ...
    endif()
endfunction()

# SOFTPN_SOURCES on Windows:
list(APPEND SOFTPN_SOURCES src/pn/l2_port_windows.cpp src/pn/l2_port_ndis.cpp)
if(SOFTPN_HAS_NPCAP)
    list(APPEND SOFTPN_SOURCES src/pn/l2_port_npcap.cpp)
endif()

softpn_windows_l2(softpn PUBLIC)                    # after add_library(softpn ...)
softpn_windows_l2(softfieldbus_shared PRIVATE)      # in the SoftFieldbus.dll block
```

### Key Concepts
1. **Dev.3**: the DLL project needs the same split (always `l2_port_windows.cpp` + `l2_port_ndis.cpp`, `l2_port_npcap.cpp` with `SOFTPN_HAS_NPCAP=1` only with the SDK) and `drivers\softpn_ndis\inc` on the include path; `tools/sync-to-dev3.ps1` mirrors that folder. This replaces the block of Phase 9.19.

---

## Step 9.20.7: Choosing the backend

### Purpose
`L2Options::backend`, `DeviceConfig::l2Backend` and a `--backend` option in the tools.

### Procedure
```
pn_probe --list [--backend auto|npcap|ndis]           # each adapter shows [ndis] or [npcap]
pn_device_demo --adapter "Ethernet 4" --name softpn-dev1 --backend ndis
pn_controller_sim --adapter "Ethernet 3" --name softpn-dev1 --backend ndis
set SOFTPN_L2_BACKEND=npcap                            # for programs without the option (service, C#)
```
Errors are explicit: "softpn_ndis driver not running (install it, then: net start SoftPnNdis)", "access denied
(the driver allows administrators and services only)", "this build has no Npcap backend".

---

## Measured results

Two Hyper-V VMs on PC-PROVE (Windows 11, Default Switch, test signing), 2026-10-17:

| Test | Result |
|---|---|
| Frames from the other VM (0x88B5 broadcast) | delivered |
| DCP Identify multicast 01:0E:CF:00:00:00 | received (`rx accepted 1 delivered 1`) |
| 802.1Q prio 6 + 0x8892 | **not received**: the Hyper-V adapter / switch drops tagged frames (VM tests use `--no-vlan 1` / `--vlan 0`); open for a physical NIC |
| `pn_probe --jitter` 8 ms, 30 s | TX max 8.50 ms (p99 8.41), RX gaps max 8.65 ms (p99 8.43), 0 lost, `send()` p99 47 us |
| `pn_device_demo` vs `pn_controller_sim`, 8 ms, 30 s | DCP, Connect, Write, Read I&M0, PrmEnd, ApplicationReady OK; `tx 1781 rx 1780, max TX period 8511 us, late 0, aborts 0`; clean release |

Found on the way: `pn_controller_sim` never called WSAStartup ("RPC: cannot create UDP socket" on Windows; it
had only run on Linux or next to a device in the same process). Fixed.

---

## Verification Checklist

- [ ] `pn_selftest` 112/112 with and without `SOFTPN_NPCAP_SDK`; without it no tool imports `wpcap.dll`
- [ ] Without the driver: `--backend ndis` reports "driver not running", `auto` falls back to Npcap
- [ ] `softpn_ndis_test selftest` 18/18 on a test VM
- [ ] Two VMs: multicast DCP received, 8 ms jitter, `pn_device_demo` vs `pn_controller_sim` in DataRun with 0 aborts
- [ ] Physical NIC (open, D4): 802.1Q-tagged RT frames, CODESYS controller, timing compared with Npcap

---

## Next Phase
This is the last phase. Update [phase-9-profinet.md](../phase-9-profinet.md) with the results.
