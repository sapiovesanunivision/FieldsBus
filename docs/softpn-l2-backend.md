# softpn: which raw-Ethernet backend to use

PROFINET RT frames are raw Ethernet frames (EtherType 0x8892), not IP packets. softpn sends and receives them
through an **L2 backend**. The rest of the stack, the GSDML and the PLC side are the same with every backend.

| Setting | Where |
|---|---|
| `L2Backend` | Dev.3 service: `Parameters\profinet\L2Backend`; `softfb::DeviceConfig::profinet.l2Backend`; SoftFieldbus.Net `ProfinetL2Backend` |
| `--backend` | `pn_device_demo`, `fb_device_demo`, `pn_probe`, `pn_controller_sim` |
| `SOFTPN_L2_BACKEND` | Environment variable, used only when the setting is `auto` (handy for a test console) |

Values: `auto` (default), `ndis`, `npcap`. Linux ignores the setting.

## The backends

| | softpn_ndis (`ndis`) | Npcap (`npcap`) | AF_PACKET (Linux) |
|---|---|---|---|
| What | Our own NDIS protocol driver (`drivers/softpn_ndis`, developed only in this repo) | The Nmap project's packet driver | Raw socket of the Linux kernel |
| Install | Driver package (D5); service `SoftPnNdis` | https://npcap.com, default options | Nothing |
| Licence | Ours | Free edition: max. 5 systems, no redistribution. Products need the Npcap OEM licence | - |
| Signing | Test-signed only today: PCs in test mode. Product signing is open ([decision](softpn-driver-signing-decision.md)) | Signed by Nmap | - |
| Rights | Administrators and services only. A `-process` console must run as administrator | All users (unless Npcap was installed "admin only") | root or `CAP_NET_RAW` |
| Tested against a PLC | CODESYS 8 ms / 32 ms (test PC 10.0.0.193, D4 and Phase 10); Driver Verifier pass | CODESYS 32 ms (dev PC); S7-1500 32 ms | `pn_controller_sim` over veth only |
| Multicast / VLAN | DCP multicast and tagged frames received; Hyper-V drops tagged frames to VMs | ALL_MULTICAST filter set by softpn; `Promiscuous` only as a fallback | joins the PROFINET multicast groups |

## Which one

| Situation | Use |
|---|---|
| Customer PC, today | **`npcap`**, on at most 5 systems with the free edition. The driver is not signed for customer PCs yet |
| Customer PC, once the driver is signed (D5) | **`ndis`**: no third-party licence, our own installer |
| Lab / test PC with the test-signed driver | **`ndis`** (as on 10.0.0.193) |
| Development PC (test signing is not allowed there) | **`npcap`** |
| Not sure what is installed | **`auto`**: the driver if it is installed **and running**, otherwise Npcap |
| Linux | nothing to choose: AF_PACKET; run as root or give the program `CAP_NET_RAW` |

Prefer an explicit `ndis` or `npcap` on a production PC. If the chosen one is missing, the service refuses to start
with a clear message, and it doesn't quietly fall back to the other one. `auto` is for mixed lab setups.

Both can be installed on the same PC; softpn opens only the one it chose. Running both drivers side by side on one
NIC is a D5 topic ([HANDOFF-softpn-ndis-d5.md](HANDOFF-softpn-ndis-d5.md)) and not verified yet.

## Messages

| Message (event log, console, `errorDescription`) | Meaning |
|---|---|
| `softpn_ndis driver not running (install it, then: net start SoftPnNdis)` | `ndis` chosen (or `auto` picked it), driver missing or stopped |
| `softpn_ndis: access denied (the driver allows administrators and services only)` | Console not elevated |
| `softpn_ndis: driver API version N, expected M` | Driver and program from different versions: update the older one |
| `Npcap is not installed (https://npcap.com); wpcap.dll not found in System32\Npcap` | `npcap` chosen, Npcap missing |
| `... - or install and start the softpn_ndis driver (net start SoftPnNdis)` | `auto` found neither |
| `unknown PROFINET L2 backend 'x' (auto, ndis, npcap)` | Typo in the setting |
| `socket(AF_PACKET): Operation not permitted (needs CAP_NET_RAW)` | Linux, not root |

To see which backend is in use: `sc query SoftPnNdis` (driver running?), or `pn_probe --backend ndis --list` /
`pn_probe --backend npcap --list` (which one lists the NIC).

## Build

- **FieldsBus (CMake):** the driver backend is always built on Windows; Npcap only when the Npcap SDK is found
  (`NPCAP_SDK` / `SOFTPN_NPCAP_SDK`). Without the SDK, `npcap` reports that the build has no Npcap backend.
- **Dev.3 (`SoftFieldbus1x.vcxproj`):** both backends always, so `NPCAP_SDK` is required ([Phase 10e](phases/dev3-profinet/Phase10e-SyncAndDllProject.md)).
  `wpcap.dll` is delay-loaded: the DLL runs on PCs without Npcap.

See also: [Phase 9.20](phases/profinet/Phase9.20-NdisBackend.md) (driver backend),
[HANDOFF-softpn-ndis-d5.md](HANDOFF-softpn-ndis-d5.md) (signing, installer, coexistence with Npcap).
