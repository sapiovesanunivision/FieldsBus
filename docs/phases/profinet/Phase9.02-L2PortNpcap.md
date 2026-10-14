# Phase 9.02: Npcap backend (Windows)

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Raw Ethernet on Windows through Npcap: adapter enumeration with MAC/IP, a BPF filter that only lets PROFINET and LLDP frames through, event-driven receive and a separate transmit handle.

## Documentation References
- [Npcap guide](https://npcap.com/guide/)
- [pcap filter syntax](https://www.tcpdump.org/manpages/pcap-filter.7.html)
- [GetAdaptersAddresses](https://learn.microsoft.com/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersaddresses)

---

## Step 9.02.1: `l2_port_npcap.cpp`

**File:** `src/pn/l2_port_npcap.cpp`

### Purpose
Implements `listAdapters()` and `openL2Port()` with Npcap. The adapter can be selected by the Npcap name, the Windows friendly name ("Ethernet 2"), the MAC or the IPv4 address.

### Implementation

```cpp
// Npcap backend of L2Port (Windows).
//
// - wpcap.dll is delay-loaded (/DELAYLOAD:wpcap.dll). Before the first pcap call we load it from
//   System32\Npcap ourselves, so a missing Npcap becomes an error message instead of a crash and
//   the global DLL search path is not touched.
// - Two pcap handles: RX (BPF-filtered, non-blocking, waited on with pcap_getevent) and TX
//   (filter that matches nothing), because a pcap_t must not be used by two threads at once.
#include "softpn/l2_port.hpp"

#include "pn_defs.hpp"

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

#include <pcap.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace softpn {
namespace {

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

// Loads Packet.dll and wpcap.dll from System32\Npcap once. Afterwards the delay-load helper finds
// the already loaded modules by name.
bool loadNpcap(std::string* error)
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        wchar_t dir[MAX_PATH];
        UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 20)
            return;
        std::wstring base(dir, n);
        base += L"\\Npcap\\";
        HMODULE packet = LoadLibraryExW((base + L"Packet.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        HMODULE wpcap = LoadLibraryExW((base + L"wpcap.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        ok = packet != nullptr && wpcap != nullptr;
    });
    if (!ok && error)
        *error = "Npcap is not installed (https://npcap.com); wpcap.dll not found in System32\\Npcap";
    return ok;
}

// Adds MAC/IP/mask/gateway from GetAdaptersAddresses to the pcap adapter list (matched by GUID).
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
    if (rc != NO_ERROR)
        return;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        std::string guid = lower(a->AdapterName ? a->AdapterName : "");
        for (AdapterInfo& info : list) {
            if (lower(info.name).find(guid) == std::string::npos || guid.empty())
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

pcap_t* openHandle(const std::string& name, bool promisc, const std::string& filter, std::string* error)
{
    char errbuf[PCAP_ERRBUF_SIZE] = {};
    pcap_t* p = pcap_create(name.c_str(), errbuf);
    if (!p) {
        if (error)
            *error = std::string("pcap_create: ") + errbuf;
        return nullptr;
    }
    pcap_set_snaplen(p, int(kMaxFrameSize + 4));
    pcap_set_promisc(p, promisc ? 1 : 0);
    pcap_set_immediate_mode(p, 1); // deliver every frame at once, no batching
    pcap_set_buffer_size(p, 4 * 1024 * 1024);
    pcap_set_timeout(p, 1);
    int rc = pcap_activate(p);
    if (rc < 0) {
        if (error)
            *error = std::string("pcap_activate(") + name + "): " + pcap_geterr(p);
        pcap_close(p);
        return nullptr;
    }
    bpf_program prog{};
    if (pcap_compile(p, &prog, filter.c_str(), 1, PCAP_NETMASK_UNKNOWN) != 0 || pcap_setfilter(p, &prog) != 0) {
        if (error)
            *error = std::string("BPF filter '") + filter + "': " + pcap_geterr(p);
        pcap_freecode(&prog);
        pcap_close(p);
        return nullptr;
    }
    pcap_freecode(&prog);
    if (pcap_setnonblock(p, 1, errbuf) != 0) {
        if (error)
            *error = std::string("pcap_setnonblock: ") + errbuf;
        pcap_close(p);
        return nullptr;
    }
    return p;
}

char hexDigit(unsigned v) { return "0123456789abcdef"[v & 0xF]; }

class NpcapPort : public L2Port {
public:
    NpcapPort(AdapterInfo info, pcap_t* rx, pcap_t* tx) : info_(std::move(info)), rx_(rx), tx_(tx)
    {
        rxEvent_ = pcap_getevent(rx_);
        interruptEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~NpcapPort() override
    {
        pcap_close(rx_);
        pcap_close(tx_);
        if (interruptEvent_)
            CloseHandle(interruptEvent_);
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
        return pcap_sendpacket(tx_, frame, int(len)) == 0;
    }

    bool receive(std::vector<uint8_t>& frame, int timeoutMs) override
    {
        const ULONGLONG deadline = GetTickCount64() + ULONGLONG(timeoutMs < 0 ? 0 : timeoutMs);
        for (;;) {
            pcap_pkthdr* hdr = nullptr;
            const u_char* data = nullptr;
            int rc = pcap_next_ex(rx_, &hdr, &data);
            if (rc == 1) {
                frame.assign(data, data + hdr->caplen);
                return true;
            }
            if (rc < 0) {
                Sleep(100); // adapter gone (driver reset): don't spin, the caller retries
                return false;
            }
            ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                return false;
            HANDLE events[2] = {rxEvent_, interruptEvent_};
            DWORD w = WaitForMultipleObjects(2, events, FALSE, DWORD(deadline - now));
            if (w == WAIT_OBJECT_0 + 1 || w == WAIT_TIMEOUT || w == WAIT_FAILED)
                return false;
        }
    }

    void interrupt() override { SetEvent(interruptEvent_); }

    const AdapterInfo& info() const override { return info_; }

private:
    AdapterInfo info_;
    pcap_t* rx_;
    pcap_t* tx_;
    HANDLE rxEvent_ = nullptr;
    HANDLE interruptEvent_ = nullptr;
    std::mutex txMutex_;
};

} // namespace

std::vector<AdapterInfo> listAdapters(std::string* error)
{
    std::vector<AdapterInfo> list;
    if (!loadNpcap(error))
        return list;
    pcap_if_t* all = nullptr;
    char errbuf[PCAP_ERRBUF_SIZE] = {};
    if (pcap_findalldevs(&all, errbuf) != 0) {
        if (error)
            *error = std::string("pcap_findalldevs: ") + errbuf;
        return list;
    }
    for (pcap_if_t* d = all; d; d = d->next) {
        if (d->flags & PCAP_IF_LOOPBACK)
            continue;
        AdapterInfo info;
        info.name = d->name ? d->name : "";
        info.description = d->description ? d->description : "";
        list.push_back(info);
    }
    pcap_freealldevs(all);
    addIpHelperInfo(list);
    // Adapters without a MAC (WAN miniports, Bluetooth PAN...) cannot carry PROFINET.
    list.erase(std::remove_if(list.begin(), list.end(), [](const AdapterInfo& a) { return a.mac.isZero(); }),
               list.end());
    return list;
}

std::unique_ptr<L2Port> openL2Port(const std::string& selector, const L2Options& options, std::string* error)
{
    std::vector<AdapterInfo> list = listAdapters(error);
    if (list.empty()) {
        if (error && error->empty())
            *error = "no Ethernet adapter found";
        return nullptr;
    }
    MacAddress mac;
    uint32_t ip = 0;
    const bool byMac = MacAddress::parse(selector, mac);
    const bool byIp = parseIpv4(selector, ip);
    const AdapterInfo* found = nullptr;
    for (const AdapterInfo& a : list) {
        if (a.name == selector || lower(a.friendlyName) == lower(selector) || (byMac && a.mac == mac) ||
            (byIp && a.ipv4 == ip)) {
            found = &a;
            break;
        }
    }
    if (!found) {
        if (error)
            *error = "adapter '" + selector + "' not found (pn_probe --list shows the names)";
        return nullptr;
    }

    // Never our own frames (Npcap loops TX back), then PROFINET (also VLAN-tagged) and LLDP.
    // The `vlan` keyword shifts the header offsets for everything after it in the expression,
    // so it must be the last term.
    std::string filter = "not ether src " + found->mac.str() + " and (ether proto 0x8892 or ether proto 0x88cc";
    if (options.extraEtherType != 0) {
        char t[8];
        t[0] = '0';
        t[1] = 'x';
        for (int i = 0; i < 4; ++i)
            t[2 + i] = hexDigit(options.extraEtherType >> (12 - 4 * i));
        t[6] = 0;
        filter += std::string(" or ether proto ") + t;
    }
    filter += " or (vlan and ether proto 0x8892))";

    pcap_t* rx = openHandle(found->name, options.promiscuous, filter, error);
    if (!rx)
        return nullptr;
    pcap_t* tx = openHandle(found->name, false, "less 1", error); // TX only: capture nothing
    if (!tx) {
        pcap_close(rx);
        return nullptr;
    }
    return std::make_unique<NpcapPort>(*found, rx, tx);
}

} // namespace softpn
```

### Key Concepts
1. **Delay-loaded `wpcap.dll`**: loaded by full path from `System32\Npcap` before the first call, so a PC without Npcap gets a clear error instead of a crash, and old `wpcap.dll` copies elsewhere are ignored.
2. **Two pcap handles**: a `pcap_t` must not be used by two threads; RX waits on `pcap_getevent()`, TX has a filter that captures nothing (`less 1`).
3. **Filter order matters**: libpcap's `vlan` keyword shifts the offsets of every term after it, so it is the last term (`not ether src <own MAC> and (... or (vlan and ether proto 0x8892))`). With `vlan` in the middle, any EtherType test after it silently never matched - found with the Phase 9.05 probe.
4. **Own frames**: Npcap loops transmitted frames back to other handles; the filter drops them.

---

## Step 9.02.2: Compile and link the Npcap backend

**File:** `CMakeLists.txt`

### Purpose
Inside `if(SOFTPN_ENABLED)`: append the platform backend after `set(SOFTPN_SOURCES ...)`, and add the Windows link settings after `softeip_warnings(softpn)`.

### Implementation

```cmake
if(WIN32)
    list(APPEND SOFTPN_SOURCES src/pn/l2_port_npcap.cpp)
else()
    list(APPEND SOFTPN_SOURCES src/pn/l2_port_linux.cpp)
endif()

# ... add_library(softpn ...) / softeip_warnings(softpn) as before ...

if(WIN32)
    target_include_directories(softpn PRIVATE "${SOFTPN_NPCAP_SDK}/Include")
    target_link_libraries(softpn PUBLIC "${SOFTPN_NPCAP_SDK}/Lib/x64/wpcap.lib" iphlpapi delayimp)
    # wpcap.dll is loaded on first use from System32\Npcap: no Npcap = error message, not a crash.
    target_link_options(softpn INTERFACE "/DELAYLOAD:wpcap.dll")
endif()
```

### Key Concepts
1. **`INTERFACE` link option**: every executable that links `softpn` gets `/DELAYLOAD:wpcap.dll`.
2. **Linux line already here**: Phase 9.03 adds `l2_port_linux.cpp`; until then build on Windows only.

---

## Verification Checklist

- [ ] `softpn` builds with 0 warnings on Windows
- [ ] After Phase 9.05, `pn_probe --list` shows every adapter with MAC, IPv4 and the `\Device\NPF_{GUID}` name
- [ ] On a PC without Npcap, `pn_probe --list` prints "Npcap is not installed ..." instead of crashing

---

## Next Phase
Continue to [Phase9.03-L2PortLinux.md](Phase9.03-L2PortLinux.md): AF_PACKET backend (Linux).
