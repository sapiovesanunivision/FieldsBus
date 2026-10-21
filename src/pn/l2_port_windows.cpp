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
        else if (error && options.backend == L2Backend::Auto && backend == L2Backend::Npcap)
            *error += " - or install and start the softpn_ndis driver (net start SoftPnNdis)";
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
