# Handoff: test pn_device_demo (PROFINET) with CODESYS Control Win

**Audience:** a new Claude Code session (or a developer) continuing the PROFINET PoC. Read this first, then
[phase-9-profinet.md](phases/phase-9-profinet.md) for the background.

## Goal of the next session

Connect the **CODESYS PROFINET IO-Controller** (CODESYS Control Win V3 x64 SP22 on PC-PROVE **10.0.0.128**) to
**`pn_device_demo`** (softpn PROFINET device) running on the development PC **10.0.0.168**, with a simple echo program
in the PLC. This is Gate B + Gate C of the PoC ([Phase 9.18](phases/profinet/Phase9.18-CodesysBringUp.md)) and the
first real-controller test of softpn. Afterwards: the same with the ProInspect process-image service, once
[Phase 9.19](phases/profinet/Phase9.19-Dev3ProcessImageService.md) is applied in Dev.3.

## State (2026-10-15)

| Item | State |
|---|---|
| FieldsBus branch | `feature/profinet-poc`, last commit `e634dd4` (PoC). **Not pushed.** |
| Uncommitted in FieldsBus | `tools/sync-to-dev3.ps1` (mirrors softpn), `tools/pack_profinet_test.ps1`, `tools/gen_profinet_docs.py`, `docs/profinet-test-runbook.md`, `docs/SoftFieldbusEipPLCSetup.md`, `docs/plc/*`, Phase 9.19 doc, results page, this file. The user decides when to commit / push. |
| softpn verified | `pn_selftest` 107/107 (Win, ASan, Linux); Linux veth test PASS; Windows Npcap device vs `pn_controller_sim` in WSL: DCP, Connect, Write, Read, PrmEnd, ApplicationReady, ModuleDiff OK; strict 24 ms watchdog not testable over WSL (36-72 ms gaps) |
| Never tested yet | softpn against a **real** PROFINET controller - this session |
| EtherNet/IP with CODESYS | Done the same day on 10.0.0.128: Generic device 32/32 vs `eip_adapter_demo` works (needed 16#97/96/64 instance IDs; timeouts with multiplier 4 because Control Win is not real-time -> use 16). EDS test with the Dev.3 service: [SoftFieldbusEipPLCSetup.md](SoftFieldbusEipPLCSetup.md), was being started (CODESYS project `E:\PLC\ProcessImage`) |
| Dev.3 | **On hold** - the user works on `dev/4524/4525_AE1931Progetti` with uncommitted files. Never switch / stash that checkout. Phase 9.19 needs a `dev/<story>/<task>` branch number from the user. |
| Npcap | Installed on 10.0.0.168 (free edition). SDK in `D:\Software\npcap-sdk-1.16`, env `NPCAP_SDK` set. `build/vs2026` configured with PROFINET. |

**Processes possibly still running on 10.0.0.168** (from the EtherNet/IP test): `ProInspectProcessImageService145_x64`
and `ProInspectProcessImageSoftFieldbusService145_x64 -process eip` (Release, started hidden by Claude) and the Debug
`..._x64d` instances (started by the user). They do not conflict with PROFINET (different ports, no Layer 2), but ask
before stopping the user's ones.

## Network facts

| | Development PC (device) | PC-PROVE (controller) |
|---|---|---|
| IP | **10.0.0.168/24, from DHCP** | 10.0.0.128/24 |
| NIC | "Ethernet", Intel I219-LM, MAC 9C-2D-CD-4D-7D-BE | "LAN" in CODESYS |
| Path | office LAN, same switch / subnet | |
| Access | local | shares `\\10.0.0.128\D` and `\\10.0.0.128\E` writable (admin shares `d$`/`e$` denied); CODESYS projects in `E:\PLC\...` |

- **DHCP matters**: the softpn device keeps the NIC's IP and refuses a DCP "Set IP" to another address (PoC rule). The
  CODESYS project must give the device exactly **10.0.0.168** (check `ipconfig` on the day; update the project if the
  lease changed), mask 255.255.255.0.
- DCP Identify and LLDP are Ethernet multicasts: everyone on the office segment receives them (harmless). Use a unique
  NameOfStation, e.g. **`softpn-dev1`**.
- The Microsoft LLDP protocol driver is not bound on "Ethernet" (nothing to do).

## Files

| What | Where |
|---|---|
| GSDML for the demo (yes, it exists) | `gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml` - DAP + modules "Input N bytes (PC to PLC)" (slot 1) and "Output N bytes (PLC to PC)" (slot 2), N = 16/32/64/128/256; VendorID 0xFFFF placeholder, DeviceID 0x0001 |
| Device binary | `build/vs2026/bin/Release/pn_device_demo.exe` (or the test kit: `pwsh tools/pack_profinet_test.ps1 -Build`) |
| Diagnostics | `pn_probe --list`, `pn_probe --adapter Ethernet --identify` |
| Runbook (general) | [profinet-test-runbook.md](profinet-test-runbook.md) - sections 0-4, 6, 8 apply |
| Reusable PLC code | `E:\PLC\testeip\PLC_PRG_PLCopen.xml` / `PLC_PRG.st` on 10.0.0.128: echo + heartbeat for 32/32 bytes, written for EtherNet/IP; works for PROFINET after adjusting the `AT %QB` / `%IB` addresses to the PROFINET module mapping (`pn_device_demo` echoes bytes 4..31 and writes a heartbeat in bytes 0..3, exactly like `eip_adapter_demo`) |

Copy the GSDML to 10.0.0.128 (e.g. into the new project folder under `E:\PLC\`) - ask the user for the folder; they
create the CODESYS projects themselves.

## Steps

**Device side (10.0.0.168):**
1. `pn_probe --list` -> "Ethernet" shows MAC 9c:2d:cd:4d:7d:be and 10.0.0.168.
2. Firewall: there is **no rule yet** for `build\vs2026\bin\Release\pn_device_demo.exe` (inbound UDP 34964 = PROFINET
   RPC). Windows asks at the first start: the user must allow it (Domain profile), or add a rule as administrator.
3. Start: `pn_device_demo --adapter Ethernet --name softpn-dev1 --in-size 32 --out-size 32`
   (start it hidden with stdout redirected to a log, as done for `eip_adapter_demo`, so Claude can read the log; the demo
   prints statistics every 10 s and every state change).

**CODESYS (10.0.0.128), new project** (user drives the GUI; Claude guides with screenshots):
1. Standard project, CODESYS Control Win V3 x64, ST. Communication: Device -> Scan -> PC-PROVE, Enter.
2. **Check first**: the PROFINET IO-Controller is available and licensed for Control Win (demo mode is fine) and which
   packet driver CODESYS uses on Windows (it may need its own driver / WinPcap / Npcap) - CODESYS help "PROFINET
   Controller", or the device's Add Device list. This was never verified.
3. Tools -> Device Repository -> Install the GSDML -> "SoftPN device" (vendor "Univision").
4. Device -> Add Device -> Ethernet (NIC "LAN", 10.0.0.128) -> **PROFINET IO Controller** -> add **SoftPN device**.
5. Slots: slot 1 **"Input 32 bytes (PC to PLC)"**, slot 2 **"Output 32 bytes (PLC to PC)"** (must equal
   `--in-size` / `--out-size`, else ModuleDiff + IOPS BAD).
6. SoftPN device -> General: station name `softpn-dev1`, IP **10.0.0.168** (not from the controller's automatic IP
   range), mask 255.255.255.0.
7. Timing: send clock 1 ms, **update time 16 or 32 ms** and a larger watchdog ("accepted update cycles without IO
   data", e.g. 10) - Control Win is not real-time (same lesson as EtherNet/IP). 8 ms with the default watchdog 3 is the
   later Gate D target, not for Control Win.
8. Ethernet -> **Scan for devices** -> SoftPN device appears (vendor 0xFFFF / device 0x0001 / "Univision SoftPN") ->
   assign the name if needed -> **Blink**: the demo log shows "DCP signal".
9. PLC code: import the testeip echo program (delete the empty PLC_PRG first, else it becomes PLC_PRG_1 and no task calls
   it), fix the `AT` addresses from the SoftPN device's **PROFINET I/O Mapping** tab.
10. F11, Login, F5.

**Watch / record:**
- Demo log: `AR: connected to '...' (MAC), update time .. ms`, `AR: Data`, `state: DataRun`, outputs changing every
  500 ms; statistics `aborts 0`.
- PLC: heartbeat counts, echo matches.
- **Wireshark capture** on 10.0.0.168 ("Ethernet"), filter `pn_dcp || pn_io || pn_rt || lldp || dcerpc`, from before the
  download until data runs, plus one STOP/RUN: this resolves the `[verify]` items (table in Phase 9.18) - DCP error
  values, RT class value, ARServerBlock, reads CODESYS issues, IOPS before ApplicationReady, RTA sequence numbers,
  LLDP Port ID.
- Results into [phase-9-profinet.md](phases/phase-9-profinet.md) and the results sheet of the runbook.

## Likely problems (and first actions)

| Symptom | First action |
|---|---|
| CODESYS scan does not find the device | restart the demo with `--promisc 1` (Npcap cannot join the DCP multicast; the I219 hardware filter may drop it) |
| "refused IP ... configure the controller with the PC's static IP" in the demo log | device IP in CODESYS != 10.0.0.168 (or the DHCP lease changed) |
| Name assignment refused | name must be lowercase / digits / '-' / '.', not `port-xyz`, not IP-like |
| Connect refused `DB 81 02 0B` | update time < 8 ms: use 16 ms (or `--min-update-ms`) |
| Connect refused with another PNIOStatus | note the 4 bytes from the demo log + capture; compare with `src/pn/pn_defs.hpp` `err::` and the Wireshark dissector |
| "expected configuration differs (ModuleDiffBlock)" | module sizes in CODESYS vs `--in-size/--out-size` |
| `ApplicationReady sent ... (retry)` repeating | CODESYS side does not answer the RPC: controller firewall / CODESYS RPC port; capture it |
| Demo stays WaitingForController after the scan | device-PC firewall blocks UDP 34964 |
| AR ends "data-hold timer expired" every few seconds | Control Win jitter: larger update time / watchdog; compare with `pn_probe --rx-jitter` |
| Read error stops the startup | a record index CODESYS reads that softpn rejects (only I&M0 0xAFF0 is implemented): see `ArManager::onRead` in `src/pn/pn_ar.cpp` |

Code changes for fixes: edit FieldsBus, rebuild (`cmake --build --preset vs2026-release`), run `pn_selftest`
(must stay 107/107 PASS), regenerate the phase docs (`python tools/gen_profinet_docs.py`) if a file shown in a phase
changed. No commit / push without the user.

## Afterwards

1. When Dev.3 is free and the user gives the branch number: apply **Phase 9.19** (sync, vcxproj block,
   `Parameters\profinet`, padding 112->128 / 252->256, `ByteOrder` none default, `.reg`, GSDML folder, installer, README),
   build all four configurations with and without `NPCAP_SDK`.
2. Then the same CODESYS test with `ProInspectProcessImageSoftFieldbusService -process profinet`: modules
   **"Input 256 bytes"** (PC -> PLC, first 252 used) and **"Output 128 bytes"** (PLC -> PC, first 112 used); the PLC
   code is `docs/plc/ProInspectEip*` with the EtherNet/IP structs (same layout), only the `AT` addresses and module
   sizes change.
3. Open items of the PoC: [phase-9-profinet.md](phases/phase-9-profinet.md) "Open items".
