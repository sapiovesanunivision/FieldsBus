# Phase 8: Dev.3 integration (SoftFieldbus DLL, UvcIOSoftModBus, process-image service)

**Goal:** use FieldsBus inside Dev.3 (ProInspect) in three places:
1. `SoftFieldbus145_x64(d).dll`, a normal UvX module in `main3.sln` (like `libmodbus`).
2. `UvcIOSoftModBus`, an IODevice plugin that reads the same `IOModBus.ini` as `UvcIOModBus` (libmodbus, LGPL),
   so ProInspect switches by changing `SystemDll`.
3. `ProInspectProcessImageSoftFieldbusService`, a copy of `ProInspectProcessImageHilscherService` with the cifX card
   replaced by `softfb::FieldbusDevice` (EtherNet/IP adapter or Modbus TCP/UDP server).

This repo stays the master copy; see `docs/dev3-integration.md` for the layout and the sync.

## 8a: FieldsBus upstream
- [x] `include/softeip/export.hpp`: `SOFTEIP_API`, `SOFTMB_API`, `SOFTFB_API`, umbrella `SOFTFIELDBUS_SHARED` /
  `SOFTFIELDBUS_BUILDING_DLL`. Exported: `Adapter`, `ModbusServer`, `ModbusClient`, `ModbusClientPoller`,
  `Result`, `resultCodeName`, `FieldbusDevice`. `C4251` disabled around the pimpl classes.
- [x] CMake option `SOFTFIELDBUS_SHARED` (`SoftFieldbus.dll`; `mb_client_test` links it), option
  `SOFTFIELDBUS_ANALYZE` (`/analyze /WX`), presets `vs2026-analyze` / `vs2026-analyze-debug`.
- [x] Code analysis fixes: `eip_adapter.cpp` 64-bit shift for the connection timeout (C6297), `mb_client.cpp`
  parentheses (C6336).
- [x] `ModbusClientPoller`: `WriteMode::OnDemand`, `flushOutputs()`, `resendOutputsOnReconnect`, `initWrites`.
- [x] `ModbusClient::abort()`; `ModbusClientPoller::stop()` uses it, so it does not wait for a response timeout.
- [x] `mb_client_test`: initWrites (with an exception reply ignored), OnDemand writes nothing by itself,
  `flushOutputs` pulse = two FC15 writes, `stop()` aborts a pending request.
- [x] `tools/sync-to-dev3.ps1`, `docs/dev3-integration.md`.

## 8b: Dev.3 SoftFieldbus DLL
- [x] Dev.3 branch `dev/4516/4517_SoftFieldbus` (user story 4516, task 4517), from `master` 97b68f3564
- [x] Sync (17 files, FieldsBus 5d9f13b); `SoftFieldbus1x.vcxproj` (+ `.filters`), `SoftFieldbus.rc`, wrapper
  `include\SoftFieldbus\SoftFieldbus.h`; added to `main3.sln` in `SDK\UvX` next to LibModBus (by hand:
  `tools\Add-ProjectToSolution.ps1` names the project after the file, makes a new GUID and does not recognise the
  `_2022` / `_2026` configurations)
- [x] Debug_2026 (code analysis), Release_2026, Debug_2022, Release_2022 build; exports checked with `dumpbin`

## 8c: UvcIOSoftModBus plugin + unit test
- [x] Plugin (`fIODevices`, `baseName "ModBus"`, `[IOModBus]` keys + optional new keys)
- [x] `UnitTestUvcIOSoftModBus` (in-process `ModbusServer`, TCP + UDP)
- [ ] IODeviceDrive (MFC dialog: needs the user) against `mb_server_demo` and EasyModbus; side by side with `UvcIOModBus`

## 8d: ProInspectProcessImageSoftFieldbusService
- [x] Service (registry `Parameters`: Transport, sizes from the Hilscher wire structs, Modbus byte order)
- [x] Diagnostics (state, errors) in the diagnostic shared memory and the event log
- [x] Installer `.iss` (compiles; not run: it installs services and needs admin rights)
- [x] `eip_scanner_sim` and `mb_client` against the service, data checked directly in the shared memory
- [ ] Installed as a Windows service with a real PLC

## Windows results 8a (2026-10-09, VS 2026, MSVC 19.51, loopback)

| Build | Result |
|---|---|
| `vs2026-analyze` Debug (`SoftFieldbus.dll`, `/analyze /W4 /WX`) | ✅ 0 warnings after the two fixes above |
| `vs2026` Release + Debug (static libs, .NET wrapper) | ✅ |
| Linux (WSL Ubuntu 24.04, g++) | ✅ 0 warnings; `mb_client_test` TCP + UDP PASS; EtherNet/IP PASS |

**`mb_client_test` against `mb_server_demo --port 1502`, linked against `SoftFieldbus.dll`:** TCP and UDP
**PASS** (0 failures). New checks:

| Check | TCP | UDP |
|---|---|---|
| `initWrites`: 0x1044 = 0xC1, 0x1043 = 0xC0 written; exception for register 5000 ignored; online | ✅ | ✅ |
| `OnDemand`: output image set to 0xFF, nothing written by the poll thread in 200 ms | ✅ | ✅ |
| `flushOutputs`: coil 40 on, off = two FC15 writes, both seen by the server | ✅ | ✅ |
| `stop()` with a request pending (server ignores the unit id, response timeout 3000 ms) | ✅ 1–14 ms | ✅ 3–4 ms |

**Found while building:** on Windows the static `softfieldbus.lib` and the DLL's import library `SoftFieldbus.lib`
are the same file name; the import library now goes to `build/<preset>/shared-lib/`.

## Dev.3 results 8b (2026-10-12, Dev.3 commit 7ce0a47cf7, local branch, not pushed)

| Configuration | Output | Result |
|---|---|---|
| Debug_2026 (v145, code analysis on 5/5 files) | `output\v145\Debug_2026\bin\SoftFieldbus145_x64d.dll`, `lib\SoftFieldbus145_x64d.lib` | ✅ 0 warnings |
| Release_2026 (v145) | `output\v145\Release_2026\bin\SoftFieldbus145_x64.dll` | ✅ |
| Debug_2022 (v143) | `output\v143\Debug_2022\bin\SoftFieldbus143_x64d.dll` | ✅ |
| Release_2022 (v143) | `output\v143\Release_2022\bin\SoftFieldbus143_x64.dll` | ✅ |
| `main3.sln -t:SDK\UvX\SoftFieldbus:Rebuild` (Debug_2026) | | ✅ |

`dumpbin /exports` (Release_2026): 67 exports; classes `softeip::Adapter`, `softfb::FieldbusDevice`,
`softmb::ModbusClient`, `ModbusClientPoller`, `ModbusServer`, `Result`. A second sync after the Dev.3 commit reports
17 unchanged (CRLF checkout does not count as an edit).

## Dev.3 results 8c (Dev.3 commit 86f6b02ab1)

`UvcIOSoftModBus145_x64(d).dll` (MFC plugin, exports `fIODevices`, depends on `SoftFieldbus145_x64(d).dll` and
`UvcStd`). All four configurations build with 0 warnings (code analysis in Debug).

`UnitTestUvcIOSoftModBus` (CppUnitTest, plugin loaded with `LoadLibrary` + `fIODevices`, in-process
`softmb::ModbusServer` on 127.0.0.1): **11/11 PASS** on Debug_2026, Release_2026 and Debug_2022.

| Test | What it checks |
|---|---|
| ConfigDefaultsAndIpAddressAsConfig | legacy defaults; config string = IP when no ini exists; UDP unit 1 |
| ConfigReadsEveryKey | every legacy and new key, hex values, clamping (InputCoils 40 -> 32), TCP unit 0, log file next to the ini |
| FactoryNamesAndGuid | base name "ModBus", same name = same device |
| OutputsInputsAndWatchDog Tcp / Udp | nothing written at open; watchdog 0x1044 = 0xC1 (+ 0x1043 = 0xC0 with DisableWatchDog); write / setLine / setLines -> coils from WriteCoilsOffset; read / getLine <- discrete inputs from ReadCoilsOffset |
| PulseIsTwoTelegrams Tcp / Udp | setLine on, off: both on the server when the calls return |
| InputsFromCoils | UseDiscreteInputs=0 reads coils (FC01); OutputCoils=0 makes write a no-op |
| OfflineOpenThenReconnects | open without server returns -2 in ~1 s, write offline returns in < 100 ms, reconnects when the server starts |
| CloseIsFast | releaseDevices < 2 s |
| InvalidLinesAndConfig | lines outside 1..32, not open, no coils configured |

## Dev.3 results 8d (Dev.3 commit 426be2a16e)

`ProInspectProcessImageSoftFieldbusService145_x64(d).exe`: all four configurations build with 0 warnings. It uses
the Hilscher wire header in place (`..\ProInspectProcessImageHilscherService\ProInspectProcessImageHilscherData.h`),
so the PLC data layout is the same (static_assert 112 / 252 bytes). Built with the compiler default (C++14): the
copied `ThreadPool.h` uses `std::auto_ptr`.

Loopback test, Debug_2026: `ProInspectProcessImageService -process` (shared memory owner) + the new service
`-process <transport> 1502`, PLC side driven by FieldsBus tools, ProInspect side read and written directly in the
shared memory (PowerShell `MemoryMappedFile`, slot 0):

| Transport | PLC -> PC (holding registers -> shared memory) | PC -> PLC (shared memory -> input registers) | Cycle |
|---|---|---|---|
| Modbus TCP | ✅ recipe "ABCDE", recipeId 0x12345678, inputs 0xA5 | ✅ recipe "XYZ", outputs 0xDEADBEEF (0xDEAD, 0xBEEF) | packetId +39 in 300 ms (10 ms) |
| Modbus UDP | ✅ same | ✅ same | ✅ |
| EtherNet/IP (`eip_scanner_sim --in-size 252 --out-size 112`) | | ✅ identity "ProInspect SoftFieldbus", Forward_Open, 300/300 packets at RPI 10 ms, packetId heartbeat | |

The installer (`ProInspectProcessImagesSoftFieldbusService145_x64.iss`) compiles with Inno Setup 6.

**Sync script** tested on an empty Dev.3-shaped folder, with PowerShell 5.1 and 7: first run adds 16 files, a
second run changes nothing, `-WhatIf` writes nothing, a hand-edited mirror file stops the sync, `-Force` overwrites
it and removes a stale file.

## 8e: PLC simulator for the process image (2026-10-12)

- FieldsBus `b547c2b`: `softeip::Scanner` (EtherNet/IP scanner / originator) in the library; `eip_scanner_sim` rebuilt
  on it (PASS on Windows, also `--skip-close`, and Linux; `/analyze` clean).
- Dev.3 `5362cf0131`: sync (2 files added) + `eip_scanner.cpp` in `SoftFieldbus1x.vcxproj`.
- Dev.3 `1d88849eac`: `ProInspectProcessImagePlcSim` (`UvpProcessImage`, in `main3.sln`): plays the PLC of the
  service over EtherNet/IP (scanner, T->O on UDP 2223) or Modbus TCP/UDP (client, holding registers 0..55 written,
  input registers 0..125 read, structs sent as they are: service `ModbusByteOrder=none`). REPL with the service's
  keys; machine mode = `viewControl[view].acquisitionControl += 1` every N ms on one view (as the
  `ProInspectProcessImageService` demo `acquire`), printing the view results. The service's `-process` takes the byte
  order too: `-process modbus-tcp 1502 none`.

Loopback test (owner `ProInspectProcessImageService -process`, service, simulator `--machine 200 --view 1`, the
ProInspect side played on the shared memory):

| Transport | acquisitionControl in the process image | view results back in the simulator |
|---|---|---|
| Modbus TCP | ✅ +5 per second | ✅ appTag 42 result 2, appTag 43 result 1 |
| Modbus UDP | ✅ | ✅ |
| EtherNet/IP | ✅ | ✅ (identity "ProInspect SoftFieldbus", Forward_Open OK) |

### 8e follow-up: registry layout, .reg files, simpler same-PC test (Dev.3 820e5c7066)

- Service registry: `Parameters` (DebugMode, Msec, Slot, CompleteAppTag, Transport, BindAddress),
  `Parameters\eip` (InputInstance, OutputInstance, ConfigInstance, MinRpiUs, ProductName),
  `Parameters\modbus` (Port, UnitId, OutputsInHoldingAt, ByteOrder). Ready-made
  `Registry\ProInspectProcessImageSoftFieldbusService_{eip,modbus_tcp,modbus_udp}.reg` (whole tree, only Transport
  differs), also installed next to the service; the installer writes the same layout.
- PLC simulator: defaults from the service's registry on the same PC, host 127.0.0.1; transport optional
  (`eip`, `modbus` = TCP); Modbus byte order `swap` supported too. Same-PC test: `ProInspectProcessImagePlcSim`.
- Tested with the built-in defaults on both sides (no registry key, no admin rights): Modbus TCP 502 with `swap`, and
  EtherNet/IP, machine mode and view results OK. Importing the `.reg` files needs admin rights (not tested here).
