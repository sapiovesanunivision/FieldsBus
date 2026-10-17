# Handoff: own Windows NDIS driver for softpn (replace Npcap)

**Audience:** a new Claude Code session (or a developer) starting the driver project. Read this first, then
[phase-9-profinet.md](phases/phase-9-profinet.md) (results, bugs found) and `include/softpn/l2_port.hpp`
(the interface the driver must serve).

Items marked **[verify]** were written from memory and must be checked before relying on them.

## Why

softpn (the in-house PROFINET RT IO-device, branch `feature/profinet-poc`) reaches raw Ethernet on Windows
through **Npcap**. The free edition is allowed for development / at most 5 systems; redistribution in a
product needs the **Npcap OEM licence (~$80k)**. An own driver removes that cost and gives control over the
receive path and timing. Alternative product answer for small volumes stays a Hilscher card (~200 EUR / station).

## State of softpn (2026-10-16)

| Item | State |
|---|---|
| Code | `include/softpn/`, `src/pn/`, tools `tools/pn_*`, demo `examples/pn_device_demo.cpp`, GSDML `gsdml/` |
| Real controller | CODESYS PN-Controller (Control Win SP22): DataRun at 32 ms, echo, STOP/RUN, restart, 30 min run OK. Siemens S7 test by a colleague in progress ([softpn-siemens-setup.md](softpn-siemens-setup.md)) |
| Self-test | `pn_selftest` 112/112 PASS (no NIC needed: memory cable + UDP loopback) |
| Commit | last commit `e634dd4`; everything since (CODESYS fixes, all-multicast, LLDP MAU, docs) is **uncommitted** - the user commits |
| Build | CMake, preset `vs2026-release`; Npcap SDK via `NPCAP_SDK` / `SOFTPN_NPCAP_SDK` (empty = no PROFINET on Windows) |

## D0 decision (2026-10-16)

| Topic | Decision | Basis |
|---|---|---|
| Approach | **Own NDIS 6.30 protocol driver** based on `Windows-driver-samples/network/ndis/ndisprot/6x` (WDM, MS-PL). XDP for Windows is dropped | XDP v1.4.0 (2026-07-29) is documented for Windows Server 2019/2022/2025 only; v1.4 moves rules to eBPF programs (eBPF for Windows = a second kernel component); production signing / redistribution not stated; the AF_XDP doc is still a TODO |
| Code location | `drivers/softpn_ndis/` in this repo, own `.sln`/`.vcxproj` (WDK MSBuild); CMake builds only the user-mode backend `src/pn/l2_port_ndis.cpp` | - |
| Test machine D1-D3 | A Hyper-V VM with Windows 11 (test signing, Secure Boot off, KDNET). D4 needs a physical PC with an Intel NIC: to be named by the user | never this dev PC or PC-PROVE |
| Signing (product) | **Open risk, management decision** (see "Signing and installation") | it decides whether the driver is cheaper than Npcap OEM or a Hilscher card |

## What the driver must provide (the L2Port contract)

The stack only uses `softpn::L2Port` (`include/softpn/l2_port.hpp`). The header already says: *"another
backend (for example an NDISPROT-based driver instead of Npcap) can be added without touching the protocol
code"*. The new backend is `src/pn/l2_port_ndis.cpp` (name free), selected at build or run time, with Npcap as
fallback. Reference implementation: `src/pn/l2_port_npcap.cpp`.

| L2Port function | Requirement | How Npcap does it today |
|---|---|---|
| `listAdapters()` | name, friendly name, MAC, IPv4/mask/gateway; skip adapters without MAC | `pcap_findalldevs` + `GetAdaptersAddresses` (keep the IP Helper part) |
| `openL2Port(selector)` | match by name, friendly name, MAC or IPv4 | as above |
| `send(frame)` | complete Ethernet frame without FCS, padded to 60 bytes, thread safe; RT/alarm frames carry an **802.1Q tag, priority 6, VID 0** inline (`kVlanTciRt = 0xC000`) | `pcap_sendpacket` on a second handle |
| `receive(frame, timeoutMs)` | one frame, one thread; **never our own transmissions**; only EtherType **0x8892** (PROFINET), **0x88CC** (LLDP), **0x8100 + inner 0x8892**, plus one optional extra EtherType (`pn_probe` uses 0x88B5) | BPF `not ether src <own> and (ether proto 0x8892 or 0x88cc [or extra] or (vlan and ether proto 0x8892))` |
| `interrupt()` | wake a blocked `receive()` | event + `WaitForMultipleObjects` |
| `link()` | speed (Mbit/s), duplex | `GetIfEntry2` (keep) |
| multicast reception | DCP Identify **01:0E:CF:00:00:00**, LLDP 01:80:C2:00:00:0E; without it an Intel I219-LM drops DCP Identify | NDIS packet filter DIRECTED + MULTICAST + **ALL_MULTICAST** + BROADCAST via `pcap_oid_set_request(OID_GEN_CURRENT_PACKET_FILTER)` |

Timing that must be met (or beaten): cyclic TX every 8-32 ms from user mode with `softeip::DeadlineTimer`;
measured with Npcap on the I219: max TX period 33.6 ms at 32 ms, `late 0`. Receive latency matters for the
data-hold watchdog (default factor 3 = 24 ms at 8 ms).

## Starting points

1. **NDISPROT sample (chosen)**: Microsoft `Windows-driver-samples` on GitHub (**MS-PL** licence). Two variants:
   `network/ndis/ndisprot/6x` (WDM, NDIS 6.0 or 6.30, INF `sys/630/ndisprot630.inf`, `ndisprot630.sys`) and
   `network/ndis/ndisprot_kmdf/60` (KMDF, NDIS 6.0 only, WDK 8 coinstaller - stale). We start from **6x / NDIS630**.
   It is an NDIS *protocol* driver: binds to the adapters, user mode opens `\\.\NdisProt` (`\Device\Ndisprot`),
   selects an adapter with `IOCTL_NDISPROT_OPEN_DEVICE`, then `ReadFile` / `WriteFile` carry raw frames.
   IOCTLs: OPEN_DEVICE, QUERY_OID_VALUE, QUERY_BINDING (enumeration), BIND_WAIT, SET_OID_VALUE.
   Test tool `prottest.exe`. INF `Class=NetTrans`, ComponentId `MS_NDISPROT`, demand start; install with
   `netcfg /l <inf> /c p /i <id>` (SetupAPI cannot install protocol drivers). Verified gaps to change:
   - **one** EtherType, compile-time `NPROT_ETH_TYPE 0x8e88` (EAPOL byte-swapped); receive skips an inline
     0x8100 tag; **send rejects** other EtherTypes, inline-VLAN frames and foreign source MACs
     -> list of EtherTypes incl. VLAN-tagged 0x8892 and an optional extra type, per open handle;
   - packet filter is DIRECTED | MULTICAST | BROADCAST without a multicast list -> add ALL_MULTICAST
     (fallback `OID_802_3_MULTICAST_LIST` 01:0E:CF:00:00:00, 01:80:C2:00:00:0E);
   - sends with `NDIS_SEND_FLAGS_CHECK_FOR_LOOPBACK` and has no loopback check on receive -> own multicasts
     come back; drop `NDIS_NBL_FLAGS_IS_LOOPBACK_PACKET` and own source MAC;
   - receive queue `MAX_RECV_QUEUE_SIZE 4` (oldest dropped) -> 256, plus drop counters;
   - overlapped I/O + several pending reads, cancel path for `interrupt()`;
   - no SDDL (`FILE_DEVICE_SECURE_OPEN` only) -> SYSTEM + Administrators;
   - link state stays in user mode (`GetIfEntry2`), no driver change.
2. **NDIS filter driver sample** (`network/ndis/filter`, a lightweight filter, LWF): the architecture Npcap
   uses. More invasive (sits in every binding stack), only if a protocol driver cannot do something we need.
3. **XDP for Windows** (`microsoft/xdp-for-windows`, MIT): Microsoft's kernel driver `xdp.sys` + AF_XDP
   sockets (`afxdp.h`) for raw receive / transmit from user mode; "generic" mode works on any NDIS interface
   (no NIC driver changes). Would replace Npcap with only a user-mode backend on our side. Docs checked on
   2026-10-16: prerequisites "Windows Server 2019 or 2022, x64", FAQ "tested on Windows Server 2019 and 2022"
   (**client Windows 10/11 not mentioned**); the built-in rule API (match by EtherType etc.) is deprecated in
   favour of **eBPF programs** (extra dependency: eBPF for Windows); production signing of `xdp.sys` and
   redistribution are not stated (the usage doc describes test signing); support 18 months per release (v1.4
   until 2028-01). **Dropped in D0** (Server only, eBPF dependency, signing unknown); no practical test planned.
4. Not usable: Windows raw sockets (no Layer 2), NDIS capture / pktmon (receive only), PacketDirect (dropped).

## Issues to expect

| Topic | Note |
|---|---|
| **VLAN tags** | NICs with "Packet Priority & VLAN" enabled (Intel default) may strip the 802.1Q tag on receive and pass it as out-of-band data (`Ieee8021QNetBufferListInfo`); on send an inline tag may be rejected or need the OOB field. softpn parses inline tags. The driver must re-insert the tag on receive (or report VID/priority) and accept inline tags on send. Test both NIC settings. |
| Coexistence | Npcap, Wireshark and the driver on the same NIC: packet filters are ORed per NDIS rules for protocols; a Wireshark capture in promiscuous mode makes the NIC promiscuous for everyone (this hid a test result once). |
| Hyper-V / WSL virtual adapters | promiscuous mode made Npcap reception stop after ~2.5 s on a vEthernet adapter; test on the physical NIC first |
| Power management | the Intel NIC had Energy Efficient Ethernet On and Interrupt Moderation Adaptive; affects jitter, not function |
| ARP / IP | not the driver's job: softpn keeps the Windows IP stack for UDP 34964 (RPC). The driver only adds a parallel Layer 2 path. |

## Signing and installation (decide early, it has lead time)

- **Development**: test signing (`bcdedit /set testsigning on`, Secure Boot off) on a test PC or VM; kernel
  debugging with WinDbg over the network (KDNET). Never on the user's development PC without asking.
- **Product** (checked 2026-10-16): Windows 10/11 load only Microsoft-signed kernel drivers. Every route needs a
  company **EV code-signing certificate** + **Microsoft Partner Center** (hardware dashboard) account.
  - Attestation signing: Microsoft retitled the page "Attestation signed drivers **for testing scenarios**"
    ([driver-signing-offerings](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings),
    2026-03-23): not Windows Certified, not on Windows Update.
  - Since the April 2026 update, Windows 11 24H2/25H2/26H1 and Server 2025 trust by default only drivers
    "properly signed through the Microsoft WHCP certification process" plus an allow-list (cross-signed trust
    removed; audit-only evaluation phase first)
    ([Windows driver policy](https://support.microsoft.com/en-US/Windows/Hardware/Drivers/the-windows-driver-policy)).
    Whether attestation-signed drivers still load is **not stated** -> test on a 24H2/25H2 PC with Secure Boot on
    after the evaluation phase, as soon as a driver exists.
  - Windows Server 2016+: only HLK-passed (WHCP) drivers.
  - Likely product path: **WHCP with HLK tests**. Enterprise alternative: an App Control for Business (WDAC)
    policy signed with the machine's Secure Boot keys (not practical for customer stations).
  - This is a management task (cost, company identity, HLK effort): raise it with the user early.
- Installer: INF for a network protocol (`Class=NetTrans`), installed with `netcfg /l <inf> /c p /i <id>`;
  uninstall `netcfg /u <id>`; coexistence with an installed Npcap.
- Tools: Visual Studio + **WDK** matching the Windows SDK; Driver Verifier; Static Driver Verifier / CodeQL.

## Suggested phases

| Phase | Content | Done when |
|---|---|---|
| D0 | Decide: XDP for Windows vs own protocol driver; signing route; where the driver code lives | **done 2026-10-16**, see "D0 decision" (signing route still a management decision) |
| D1 | Toolchain: WDK install, test PC/VM with test signing + KDNET; build the **unmodified** NDISPROT sample, install, send/receive with its test tool | **done 2026-10-16**, see "D1 result" |
| D2 | Driver changes: EtherType list + VLAN, packet filter per open, no loopback, overlapped reads, cancel, enumeration, link state, SDDL | **send side + API done 2026-10-16** (selftest 18/18 in the VM); receive test and Driver Verifier open, see "D2 result" |
| D3 | `src/pn/l2_port_ndis.cpp` backend + build switch + fallback to Npcap; `pn_probe --list/--identify/--jitter` on it | **done 2026-10-17**, see "D3 result" |
| D4 | Real test: CODESYS on 10.0.0.128 (procedure: [HANDOFF-profinet-codesys.md](HANDOFF-profinet-codesys.md)), then Siemens; compare timing with Npcap (`pn_probe --jitter`, demo statistics) | DataRun, `aborts 0`, timing equal or better than Npcap |
| D5 | Signing, INF/installer, docs (phase doc like `docs/phases/profinet/`), README | signed driver installs on a clean Windows 11 PC |

### D1 result (2026-10-16)

- **Build** (this PC):
  - WDK from NuGet: `drivers/softpn_ndis/packages.config`, restored with
    `nuget restore packages.config -PackagesDirectory packages` (10.0.28000.2526, 1.5 GB, gitignored).
  - Add the VS "Windows Driver Kit" component. On this PC it sits in VS 2022; VS 2026 redirects the
    `WindowsKernelModeDriver10.0` toolset to v170.
  - Build with `drivers/softpn_ndis/build.ps1`.
  - Workarounds:
    - `WDKBuildFolder` / `WDKBinRoot` are set in `Directory.Build.props`.
    - The **64-bit** MSBuild is required: with the 32-bit one, stampinf/InfVerif/ApiValidator are searched in `bin\x86`.
    - Debug builds without Spectre libs (`Directory.Build.targets`). **Release needs "MSVC v143 Spectre-mitigated libs"**.
- **Result:** `ndisprot630.sys` + INF + catalog, test-signed. Neither `prottest.exe` nor the driver needs the CRT.
- **Kit:** `drivers/softpn_ndis/pack_kit.ps1` (copies the driver, the `.cer`, prottest and `install.cmd` / `uninstall.cmd`).
- **VM** `pndev` on PC-PROVE (Hyper-V Gen2, Windows 11, Secure Boot off, test signing, Default Switch):
  - `install.cmd` worked; `prottest -e` lists the Hyper-V adapter.
  - `prottest -w -n 20 -m ff:ff:ff:ff:ff:ff '\DEVICE\{GUID}'` sent 20/20 EAPOL frames (100 bytes), seen on
    the host's vEthernet (Default Switch): `build/test2.pcapng`.
- **Gotchas:**
  - prottest's default destination is the **own MAC**, and the Hyper-V switch does not forward that to the
    host: use `-m ff:ff:ff:ff:ff:ff`.
  - In PowerShell, quote `'\DEVICE\{GUID}'` (the braces are a script block).
- **Not done yet:** KDNET / kernel debugger (needed for D2 under Driver Verifier), the receive test (`prottest -r`
  needs a second sender).

### D2 result (2026-10-16)

- **Driver:** `drivers/softpn_ndis/sys/` (`softpn_ndis.sys`, derived from `upstream/`, changes marked `softpn:`).
  - Interface in `drivers/softpn_ndis/inc/softpn_ndis_ioctl.h` (shared with user mode):
    - device `\\.\SoftPnNdis`, SDDL SYSTEM + Administrators;
    - the sample IOCTLs, plus `GET_VERSION`, `SET_FILTER` (up to 8 EtherTypes, flags VLAN / DROP_OWN) and `GET_STATS`.
  - Service, `Ndi\Service` and the NDIS protocol name are all `SoftPnNdis`; INF component `UV_SoftPnNdis`.
  - Receive:
    - every accepted frame is copied, and the miniport NBLs are returned at once;
    - an OOB 802.1Q tag is re-inserted inline;
    - loopback frames and frames with the own source MAC are dropped;
    - queue 256 (was 4).
  - Send:
    - only the filter's EtherTypes (also behind an inline tag) and the own source MAC;
    - no `NDIS_SEND_FLAGS_CHECK_FOR_LOOPBACK`.
  - Packet filter: + ALL_MULTICAST, with a fallback to the multicast list 01:0E:CF:00:00:00 / 01:80:C2:00:00:0E.
  - Fixed a reference leak of the sample (receive while paused).
- **Build:** `/W4 /WX` clean, InfVerif clean. Code analysis shows only the 6 warnings the unmodified sample has too,
  among them `PartialCancelId` (a UCHAR shifted by 56, so always 0; harmless, cancel IDs are per binding).
- **Test tool:** `drivers/softpn_ndis/test/softpn_ndis_test.exe` (static CRT): `list`, `selftest`, `send`, `recv`.
- **VM `pndev`:** `install.cmd` OK, and `selftest 0` 18/18 PASS:
  - filter checks; send 0x88B5 and 802.1Q prio 6 + 0x8892; refused 0x0800 / foreign source / runt;
  - no own frames; `CancelIoEx`; single open per adapter; counters; reopen.
- **Deferred:**
  - VLAN OOB on send (Npcap sends inline tags on the I219 successfully);
  - a registry setting for the queue size.
- **Still open:**
  - receive test with a second sender (multicast DCP / LLDP, VLAN re-insert);
  - Driver Verifier run with a kernel debugger attached.

### D3 result (2026-10-17)

- **Code:**
  - `src/pn/l2_port_windows.{hpp,cpp}`: dispatcher plus the IP Helper / link / adapter selection code both backends share.
  - `src/pn/l2_port_ndis.cpp`: one overlapped handle per adapter, 4 reads posted in the driver, writes with a 1 s timeout.
  - `src/pn/l2_port_npcap.cpp`: backend only.
  - `L2Backend` Auto/Npcap/Ndis: `L2Options::backend`, `DeviceConfig::l2Backend`, env `SOFTPN_L2_BACKEND`.
    `Auto` = the driver if `\\.\SoftPnNdis` opens, else Npcap.
  - `--backend` option in `pn_probe`, `pn_controller_sim` and `pn_device_demo`.
  - `pn_probe --list` shows `[ndis]` / `[npcap]`.
- **Build:**
  - On Windows softpn always builds. Npcap is compiled in only with `SOFTPN_NPCAP_SDK` (`SOFTPN_HAS_NPCAP`).
  - Without the SDK the tools need no `wpcap.dll`.
  - `pn_selftest` 112/112 in both builds.
- **Bug found and fixed:** `pn_controller_sim` never called WSAStartup ("RPC: cannot create UDP socket" on Windows).
  It had only run on Linux or next to a device in the same process.
- **Two VMs on PC-PROVE** (`pndevA` `Ethernet 3` 00:15:5d:00:80:01, `pndevB` `Ethernet 4` 00:15:5d:00:80:02, Default
  Switch; the old `pndev` lost its middle checkpoint disk after a manual file move and was rebuilt):

| Test | Result |
|---|---|
| Receive from another machine | 0x88B5 broadcasts A -> B delivered |
| DCP Identify multicast 01:0E:CF:00:00:00 (ALL_MULTICAST) | received on B, `rx accepted 1 delivered 1` |
| 802.1Q prio 6 + 0x8892 | **not received** (`rx accepted 0`), A sent 50 without error: the Hyper-V adapter / switch drops tagged frames. Open for D4 on a physical NIC |
| Timing 8 ms (`pn_probe --jitter` / `--rx-jitter`) | TX max 8.50 ms, p99 8.41; RX gaps max 8.65 ms, p99 8.43; 0 lost; `send()` p50 16 us, p99 47 us |
| `pn_device_demo` (B) vs `pn_controller_sim` (A), untagged (`--no-vlan 1` / `--vlan 0`) | DCP, Connect, Write, Read I&M0, PrmEnd, ApplicationReady OK; DataRun at 8 ms, 30 s: `tx 1781 rx 1780, max TX period 8511 us, late 0, aborts 0`; clean release |

- **Open:**
  - VLAN-tagged frames on a physical NIC (D4);
  - Driver Verifier run with a kernel debugger;
  - phase doc `Phase9.20-NdisBackend.md` + `gen_profinet_docs.py` entry;
  - D4 with CODESYS on a physical PC.

Optional later: cyclic transmit scheduled in the driver (kernel timer) if user-mode jitter limits the 8 ms /
watchdog-3 target (Gate D).

## Test bed and rules (from the PROFINET PoC)

- Device PC 10.0.0.168 (this PC, DHCP, "Ethernet" Intel I219-LM, MAC 9c:2d:cd:4d:7d:be; Npcap installed).
  PC-PROVE 10.0.0.128: CODESYS Control Win V3 x64 SP22 with the PN-Controller (demo mode **stops after 2 h**),
  shares `\\10.0.0.128\D` / `\\10.0.0.128\E`. Wireshark portable: `D:\Assistenza\WiresharkPortable\App\Wireshark\`
  (tshark 2.2.6, old field names).
- Start the demo: `pn_device_demo --adapter Ethernet --name softpn-dev1 --in-size 32 --out-size 32`
  (no `--promisc` needed any more).
- Regression capture of a CODESYS startup: `tests/profinet/captures/codesys-sp22-startup-32ms.pcapng`.
- After code changes: `cmake --build --preset vs2026-release`, `pn_selftest` must stay PASS, regenerate the
  phase docs with `python tools/gen_profinet_docs.py` if a file shown in a phase changed.
- **No commit / push without the user.** Kernel-driver installs, test signing and boot configuration changes
  only on a machine the user names, after asking.
- Dev.3 (`D:\Univision\Dev.3`) is on hold for PROFINET (Phase 9.19); never switch or stash its checkout.

## First steps for the new session

1. Read this file, `include/softpn/l2_port.hpp`, `src/pn/l2_port_npcap.cpp`, `docs/phases/phase-9-profinet.md`.
2. Research and verify the **[verify]** items (sample paths, signing rules); XDP for Windows: see starting
   point 3 (docs already checked, only the practical Windows 11 test is open).
3. Present the D0 decision (XDP vs NDISPROT, signing, code location) to the user with a recommendation.
4. Ask which machine may be used for test signing / kernel debugging before installing anything.
