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
- [ ] Dev.3 branch `dev/<story>/<task>`
- [ ] Sync; `SoftFieldbus1x.vcxproj`, `.rc`, wrapper `include\SoftFieldbus\SoftFieldbus.h`; add to `main3.sln` (UvX)
- [ ] Debug_2026 (code analysis), Release_2026, Debug_2022, Release_2022 build; exports checked with `dumpbin`

## 8c: UvcIOSoftModBus plugin + unit test
- [ ] Plugin (`fIODevices`, `baseName "ModBus"`, `[IOModBus]` keys + optional new keys)
- [ ] `UnitTestUvcIOSoftModBus` (in-process `ModbusServer`, TCP + UDP)
- [ ] IODeviceDrive against `mb_server_demo` and EasyModbus; side by side with `UvcIOModBus`

## 8d: ProInspectProcessImageSoftFieldbusService
- [ ] Service (registry `Parameters`: Transport, sizes from the Hilscher wire structs, Modbus byte order)
- [ ] Diagnostics (state, errors) in the diagnostic shared memory and the event log
- [ ] Installer `.iss`
- [ ] `eip_scanner_sim` and `mb_client poll` against the service, checked with the process-image viewer

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

**Sync script** tested on an empty Dev.3-shaped folder, with PowerShell 5.1 and 7: first run adds 16 files, a
second run changes nothing, `-WhatIf` writes nothing, a hand-edited mirror file stops the sync, `-Force` overwrites
it and removes a stale file.
