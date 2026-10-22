# Phase 10: Dev.3: PROFINET in the process-image service, byte order for all transports

**Goal:** finish what Phase 8 did for EtherNet/IP and Modbus. `ProInspectProcessImageSoftFieldbusService` should
also run as a **PROFINET IO-Device** (`Transport=profinet`), and every transport gets a **byte-order** option for
its WORD, DWORD and float fields (Siemens S7 needs big-endian). The PLC side gets the same kit as EtherNet/IP:
a GSDML for the service, a CODESYS program (`ProInspectPn_PLCopen.xml`) and a TIA program (`ProInspectPn.scl`).

This phase replaces [Phase 9.19](profinet/Phase9.19-Dev3ProcessImageService.md), which was written before the own NDIS driver
([Phase 9.20](profinet/Phase9.20-NdisBackend.md)) existed and had a PROFINET-only `none|swap` flag.

- Dev.3 branch: `dev/4516/4526-ProcessImage` (story 4516, task 4526)
- FieldsBus branch: `feature/profinet-poc`
- **Step-by-step documents: [dev3-profinet/README.md](dev3-profinet/README.md)** (10a-10j, copyable code)
- **Status: planned.** The code in the step documents is a proposal, written against the sources of 2026-10-21
  (Dev.3 `8ab48fff1a`, FieldsBus `65cb2a0`). Nothing has been built or tested yet.

## Decisions

| Topic | Decision | Why |
|---|---|---|
| Byte orders | `little`, `big` (ABCD), `big-wordswap` (CDAB); `none` = `little` and `swap` = `big` stay valid | `none`/`swap` are the values used today. CDAB (each WORD big-endian, DWORD/float low word first) is a common Modbus variant |
| Where | One `ByteOrder` value in each transport subkey: `eip`, `modbus`, `profinet` | Matches the existing registry layout; switching `Transport` keeps the right order for each protocol |
| Defaults | eip **little**, modbus **big** (as today), profinet **big** | Rockwell / CODESYS EtherNet/IP are little-endian; Modbus registers are big-endian; Siemens is the main PROFINET target |
| PROFINET modules | **256 bytes in each direction** (existing `IN256` 0x14 / `OUT256` 0x24). The 112 / 252 bytes are at offset 0, the rest is zero | softpn accepts only the GSDML sizes 16..256 (`src/pn/pn_model.cpp`); one size is simple for the PLC engineer; no change to softpn needed |
| Raw Ethernet | **Npcap and softpn_ndis, both always built**, chosen at run time (`profinet\L2Backend` = auto / ndis / npcap; guidance: [softpn-l2-backend.md](../softpn-l2-backend.md)). The Npcap SDK becomes a Dev.3 build prerequisite | The driver is not signed yet (D5), so Npcap stays the way to run on a normal PC. `auto` = the driver if it is installed and running, otherwise Npcap |
| Driver development | **softpn_ndis stays in FieldsBus only** (sources, build, signing, package). Dev.3 gets only the user-mode backend (`src/pn/l2_port_ndis.cpp`) and the interface header `drivers/softpn_ndis/inc/softpn_ndis_ioctl.h` through the sync; the driver is installed by its own package (D5), not by the service setup | One home for the kernel code and its signing; the backend's API version check catches a DLL / driver mismatch |
| Identity | Own GSDML "ProInspect SoftFieldbus", VendorID 0xFFFF (placeholder), **DeviceID 0x0002**, both modules `FixedInSlots` | The generic SoftPN GSDML (DeviceID 1) offers every size; the service needs exactly one layout |
| PLC programs | CODESYS: byte copy from module arrays mapped without `AT`; TIA: `DPRD_DAT` / `DPWR_DAT` into standard-access structs. Both check the byte order through the `packetId` step | Same patterns as the verified echo programs; a wrong `ByteOrder` is the most likely commissioning error |

## Sub-phases

| Phase | Title | Repository |
|---|---|---|
| [10a](dev3-profinet/Phase10a-L2BackendOption.md) | L2 backend option in `softfb::DeviceConfig` (+ .NET, `fb_device_demo`, error text) | FieldsBus |
| [10b](dev3-profinet/Phase10b-ProInspectGsdml.md) | GSDML for the ProInspect service, `pn_selftest` check | FieldsBus |
| [10c](dev3-profinet/Phase10c-CodesysProgram.md) | CODESYS PLC program (`ProInspectPn.st`, `_PLCopen.xml`) | FieldsBus |
| [10d](dev3-profinet/Phase10d-TiaProgramAndGuide.md) | TIA PLC program (`ProInspectPn.scl`), `SoftFieldbusPnPLCSetup.md` | FieldsBus |
| [10e](dev3-profinet/Phase10e-SyncAndDllProject.md) | Sync-script fix, sync, DLL project with both backends | FieldsBus + Dev.3 |
| [10f](dev3-profinet/Phase10f-ByteOrder.md) | Byte order for all transports (`ProcessImageByteOrder.h`) | Dev.3 |
| [10g](dev3-profinet/Phase10g-ProfinetTransport.md) | PROFINET transport in the service | Dev.3 |
| [10h](dev3-profinet/Phase10h-RegistryGsdmlInstaller.md) | Registry files, GSDML folder, installer | Dev.3 |
| [10i](dev3-profinet/Phase10i-ReadmeAndPlcSim.md) | Service README, PLC simulator | Dev.3 |
| [10j](dev3-profinet/Phase10j-BuildAndTest.md) | Build and test | both |

**Found while writing the step documents:** `tools/sync-to-dev3.ps1` maps `drivers/softpn_ndis/inc` but does not
export it (`git archive ... include src`), so the first sync with softpn would stop with "HEAD has no
drivers/softpn_ndis/inc". The fix is step 10e.1. A dry run with the fix at `65cb2a0` gives
added 32, changed 5, removed 0.

**Added after the first soak (2026-10-22):** the service had no frame statistics, so a dropout shorter than the PLC
watchdog was invisible. FieldsBus `6358f5b`: `FieldbusDevice::profinetStats()` (softpn tx/rx, late cycles, max TX
period, connects, aborts, last abort reason; thread safe against `stop()`), `fb_device_demo` prints it. Dev.3 service:
key `t` shows it, registry `Parameters\LogStats` = minutes between statistics lines in the event log (0 = off; a line
with a new abort is a warning), plus the copy-cycle timing for every transport. PLC programs: `nPcLost` / `udPcLost`
counts every `bPcAlive` TRUE -> FALSE.

## Checklist

- [ ] 10a `l2Backend` in `softfb::DeviceConfig`, .NET, `fb_device_demo`; error text names both backends
- [ ] 10b ProInspect GSDML; `pn_selftest` 129/129; imports in TIA and CODESYS
- [ ] 10c / 10d PLC programs and `SoftFieldbusPnPLCSetup.md`; FieldsBus committed and pushed (with 10e.1)
- [ ] 10e Sync; vcxproj (+ filters) with both backends and the Npcap SDK check
- [ ] 10f `ProcessImageByteOrder.h`; per-transport `ByteOrder`; command line; `swapForModbus` removed
- [ ] 10g `Parameters\profinet` incl. `L2Backend`; 256 / 256 modules
- [ ] 10h `.reg` (4, UTF-16), GSDML folder, installer task + ProgramData folder + prerequisite warning
- [ ] 10i README, PLC simulator
- [ ] 10j builds, regression x byte orders, CODESYS (both backends), Siemens; results below; Phase 9 open item closed

## Results

### Builds (10j.1)

| Build | Result |
|---|---|
| FieldsBus `vs2026-release`, `vs2026-analyze-debug` | ✅ 0 warnings |
| FieldsBus `pn_selftest` | ✅ 129/129 Windows and Linux |
| FieldsBus Linux (WSL Ubuntu 24.04, g++ 13.3, `b5c8e1f`) | ✅ 16 targets, 0 warnings; `--backend` validated and otherwise ignored (AF_PACKET; as root: device up on eth0) |
| Dev.3 SoftFieldbus DLL, Debug / Release 2026 (also builds in 2022) | ✅ 0 warnings |
| Dev.3 service, PLC simulator, `UvcIOSoftModBus`, Debug / Release 2026 | ✅ 0 warnings (2022 configurations no longer supported for these, 2026-10-22) |
| `UnitTestUvcIOSoftModBus` | ✅ 11/11 Debug and Release 2026 |
| Dev.3 without `NPCAP_SDK` (expected: build error with the Npcap message) | ✅ |
| Installer compiles | ✅ |
| Installer and `.reg` files on a VM (admin rights): task, registry values, ProgramData folder, Npcap / driver warning, transport switch | ✅ 2026-10-22 (user) |

### Regression and byte order (10j.2, loopback with the PLC simulator)

| Transport | little | big | big-wordswap | mismatch detected |
|---|---|---|---|---|
| EtherNet/IP | ✅ | ✅ | ✅ | ✅ (vs big: 0xD2040000) |
| Modbus TCP | ✅ | ✅ | ✅ | ✅ (big vs big-wordswap: 0x04D20000) |
| Modbus UDP | ✅ | ✅ | ✅ | ✅ (big-wordswap vs little: 0x0000D204) |

2026-10-22, Release_2026, dev PC loopback; ✅ = recipeId 1234 and inputs 3 at the service, packetId rising at the
simulator. Details: [10i results](dev3-profinet/Phase10i-ReadmeAndPlcSim.md#verification-checklist).

### PROFINET (10j.3, 10j.4)

| PLC | Backend | Byte order | ConnectedRun | packetId / `bByteOrderOk` | recipe / recipeId / measurements | STOP/RUN | Soak |
|---|---|---|---|---|---|---|---|
| CODESYS Control Win (10.0.0.128), `ProInspectPn_PLCopen.xml` | Npcap (dev PC 10.0.0.168), `-process profinet 0 little Ethernet` | little | ✅ 2026-10-21 | ✅ packetId changes (negative half the time: 16-bit wrap) | ✅ PC sees recipeId 1234, inputs counting, recipe "SOFTFIELDBUS TEST" | | |
| CODESYS Control Win, same | softpn_ndis (test PC 10.0.0.193), registry `L2Backend=ndis` | little | ✅ 2026-10-22 | ✅ | ✅ | ✅ | ✅ 32 ms: 1 h, no error. 8 ms (installed service, `LogStats`=10): after 40 min tx 300 039 / rx 300 024, late 0, connections 1, aborts 0, max TX period 11.7 ms; per 10 min tx +75 024 / rx +75 022; copy cycle mean 10.00 ms, max 14.07 ms |
| Siemens S7-1500, `ProInspectPn.scl` | softpn_ndis or Npcap | big | | | | | |

Dev.3 commits: (to fill)
