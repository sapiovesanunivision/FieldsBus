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
