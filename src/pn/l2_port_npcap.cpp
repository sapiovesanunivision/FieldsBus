// Npcap backend of L2Port (Windows), chosen by l2_port_windows.cpp.
//
// - wpcap.dll is delay-loaded (/DELAYLOAD:wpcap.dll). Before the first pcap call we load it from
//   System32\Npcap ourselves, so a missing Npcap becomes an error message instead of a crash and
//   the global DLL search path is not touched.
// - Two pcap handles: RX (BPF-filtered, non-blocking, waited on with pcap_getevent) and TX
//   (filter that matches nothing), because a pcap_t must not be used by two threads at once.
#include "l2_port_windows.hpp"

#include "pn_defs.hpp"

#include <pcap.h>

#include <cstring>
#include <mutex>

namespace softpn {
namespace {

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

// NDIS packet filter (ntddndis.h), set through Npcap like pcap_set_promisc() does internally.
constexpr bpf_u_int32 kOidGenCurrentPacketFilter = 0x0001010E;
constexpr ULONG kNdisPacketTypeDirected = 0x00000001;
constexpr ULONG kNdisPacketTypeMulticast = 0x00000002;
constexpr ULONG kNdisPacketTypeAllMulticast = 0x00000004;
constexpr ULONG kNdisPacketTypeBroadcast = 0x00000008;

// Receives every multicast frame without promiscuous mode. The NIC's multicast filter only passes
// the groups the IP stack joined, so DCP Identify (01:0E:CF:00:00:00) is dropped otherwise (seen on
// an Intel I219-LM). Npcap ORs this with the filters of the other users of the NIC. Best effort: if
// the driver refuses it, DCP Identify needs --promisc as before.
bool requestAllMulticast(pcap_t* p)
{
    ULONG f = kNdisPacketTypeDirected | kNdisPacketTypeMulticast | kNdisPacketTypeAllMulticast | kNdisPacketTypeBroadcast;
    size_t len = sizeof f;
    return pcap_oid_set_request(p, kOidGenCurrentPacketFilter, &f, &len) == 0;
}

pcap_t* openHandle(const std::string& name, bool promisc, bool allMulticast, const std::string& filter,
                   std::string* error)
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
    if (allMulticast && !promisc)
        (void)requestAllMulticast(p);
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
        haveLuid_ = win::luidFromGuidName(info_.name, luid_);
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

    LinkStatus link() const override { return haveLuid_ ? win::linkStatus(luid_) : LinkStatus{}; }

private:
    AdapterInfo info_;
    NET_LUID luid_{};
    bool haveLuid_ = false;
    pcap_t* rx_;
    pcap_t* tx_;
    HANDLE rxEvent_ = nullptr;
    HANDLE interruptEvent_ = nullptr;
    std::mutex txMutex_;
};

} // namespace

std::vector<AdapterInfo> npcapListAdapters(std::string* error)
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
        info.backend = "npcap";
        list.push_back(info);
    }
    pcap_freealldevs(all);
    win::addIpHelperInfo(list);
    return list;
}

std::unique_ptr<L2Port> npcapOpen(const AdapterInfo& adapter, const L2Options& options, std::string* error)
{
    // Never our own frames (Npcap loops TX back), then PROFINET (also VLAN-tagged) and LLDP.
    // The `vlan` keyword shifts the header offsets for everything after it in the expression,
    // so it must be the last term.
    std::string filter = "not ether src " + adapter.mac.str() + " and (ether proto 0x8892 or ether proto 0x88cc";
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

    pcap_t* rx = openHandle(adapter.name, options.promiscuous, true, filter, error);
    if (!rx)
        return nullptr;
    pcap_t* tx = openHandle(adapter.name, false, false, "less 1", error); // TX only: capture nothing
    if (!tx) {
        pcap_close(rx);
        return nullptr;
    }
    return std::make_unique<NpcapPort>(adapter, rx, tx);
}

} // namespace softpn
