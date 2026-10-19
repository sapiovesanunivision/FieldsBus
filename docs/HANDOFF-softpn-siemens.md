# Handoff: SoftPN with a Siemens S7-1500 (TIA Portal)

**Audience:** a new Claude Code session (or a developer) continuing the Siemens test of softpn. Read this first, then
[softpn-tia-quickstart.md](softpn-tia-quickstart.md) (what the tester follows) and
[softpn-siemens-setup.md](softpn-siemens-setup.md) (full guide, troubleshooting table).

## Context

- softpn = in-house PROFINET RT IO-device (CC-A, RT_CLASS_1), branch `feature/profinet-poc`, code in
  `include/softpn/`, `src/pn/`, tools `tools/pn_*`, demo `examples/pn_device_demo.cpp`.
- Works with the **CODESYS** PN-Controller (Control Win SP22, PC-PROVE 10.0.0.128): DataRun at 32 ms, echo,
  STOP/RUN, restart, 30 min, 0 aborts (2026-10-15/16). Procedure: [HANDOFF-profinet-codesys.md](HANDOFF-profinet-codesys.md).
- **Siemens is the first second-vendor test.** A colleague of the user runs it on site; the user relays files,
  screenshots and logs and is often **remote**: send files with `SendUserFile` (not only a path on disk), and paste
  short files inline as a fallback.
- Everything since commit `e634dd4` is **uncommitted** (the user commits). No commit / push without the user.

## Test bed (Siemens side)

| Item | Value |
|---|---|
| CPU | CPU 1511-1 PN, 6ES7 511-1AK00-0AB0, HW 5, **FW V1.8.5** |
| TIA Portal | V18 (from the screenshots, `TIA V18` in the taskbar) [verify] |
| Network | PLC subnet `192.168.10.x` (TIA PC = `192.168.10.253`); PLC IP [verify] |
| Device PC | the colleague's Windows PC with Npcap and the SoftPN test kit; kit build date unknown [verify] |
| TIA project | `F:\Work\Commesse\Univision-150C\EthernetPN_Test\tia_proj\TiaEthPNtest_1` (colleague's PC) |
| Device in TIA | name `softpn-device`, modules Input 32 / Output 32 bytes, FB `SoftPN_Echo` |

## Done (2026-10-16)

1. **GSDML accepted by TIA Portal** after 4 rounds (CODESYS never checked these). File name unchanged:
   `gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml`. Fixes: `NameOfStationNotTransferable="false"`;
   `schemaLocation` with backslashes; `MinDeviceInterval="256"` (8 ms); explicit
   `ReductionRatio="1 2 4 8 16 32 64 128 256 512"` (a list starting at 8 is refused, no list fails another check);
   `PTP_BoundarySupported` / `DCP_BoundarySupported="true"`; `NumberOfDeviceAccessAR` removed. Table with the TIA
   check numbers: [softpn-siemens-setup.md](softpn-siemens-setup.md) section 4.1. `pn_selftest` 112/112 after each.
2. **PLC program:** [plc/SoftPN_Echo.scl](plc/SoftPN_Echo.scl), an FB with inputs `hwIn` / `hwOut` (HW_IO)
   instead of hard-coded module names (the guide's example names did not exist in the colleague's project and he
   used `softpn-device~X1`, the interface, by mistake). Import via External source files -> Generate blocks.
3. **Quick start for the tester:** [softpn-tia-quickstart.md](softpn-tia-quickstart.md), sent to the user
   together with the SCL file and the GSDML.

## Current problem (open)

**The PLC does not find the device:**

- CPU diagnostics buffer (`PLC_Diagnostics_1_2.txt`, 145 entries) after every STOP->RUN / power-on:
  `16#02:39CB IO device failure - IO device not found (softpn-device)`.
- Also, repeatedly on `PLC_1..Port_1`: `16#06:4040 Error on partner - Wrong partner` / `Wrong partner port` and
  sometimes `No neighbor could be detected` -> a **topology (port interconnection) is configured** in TIA and the
  LLDP neighbour the CPU sees differs. Likely a switch or another device between PLC and PC, or the interconnection
  points to the SoftPN port while something else is cabled.
- The user reports **`pn_device_demo` "always with 0 values"** (the `stats:` line: tx/rx 0): the demo receives
  **no PROFINET frames from the PLC at all**, so it never sees the DCP Identify. The problem is before the stack:
  NIC / cabling / multicast / Npcap.

Asked the user to check, in this order (no answer yet):

1. `pn_probe --list`: is `--adapter` the NIC cabled to the PLC, with an IP in `192.168.10.x`?
2. Start with `--promisc 1`. Kits built **before 2026-10-16** have no NDIS ALL_MULTICAST request, and an Intel
   I219 then drops the DCP Identify multicast `01:0E:CF:00:00:00` (seen on our test PC). Rebuilding the kit is the
   real fix: `tools/pack_profinet_test.ps1 -Build -Destination <folder>`.
3. TIA Online -> Accessible devices: does "Univision SoftPN" appear (with the PC's MAC)? Then Assign device name.
4. Delete the port interconnection in the Topology view (or partner "any").
5. Wireshark on the device PC during STOP->RUN, filter `pn_dcp || lldp` (+ the demo console output).

### Update 2026-10-17 (log `build/pow_log_1.txt`, old kit of 2026-10-15)

- Without `--promisc 1`: stats 0, as before. **With `--promisc 1` the S7 finds the device and connects**
  (`plcxb1d0ed`, 28:63:36:83:f9:9e, 192.168.10.1 -> device 192.168.10.10, 16 ms, C_SDU 40/40, output FrameID 0x8002,
  no ModuleDiff). So the old kit lacked ALL_MULTICAST.
- Then a loop: `AR: WaitPrmEnd`, and about 4.6 s later a new Connect with the same ARUUID (`aborts` +1), 52 times.
  `rx 0` the whole time: the S7 never sends output frames, no PrmEnd, no RTA ERR. So the S7 **does not accept
  our Connect response** (or does not receive it). Writes/Reads were not logged, so we can't tell if any came.
- Changes (selftest 112/112, kit `softpn-testkit-20261017.zip` sent to the user):
  1. ARBlockRes `CMResponderUDPRTPort` = 0x8892 (was the RPC port 0x8894). This is the main suspect.
  2. RPC socket bound to the NIC IP (it was INADDR_ANY): if the PC has a second NIC/IP in 192.168.10.x, replies
     could leave with the wrong source IP.
  3. Console now logs every record Write/Read (slot, subslot, index, status), refused Control blocks and rejected RPC calls.
- If the loop persists: Wireshark capture (filter `pn_dcp || pn_io || pn_rt || dcerpc || lldp || arp`) during one
  Connect cycle, plus `ipconfig /all` of the device PC. Then check the ARServerBlock (S7-1500 uses Advanced startup,
  GSDML has `StartupMode="Advanced;Legacy"`) and the RPC response header.

## Code facts relevant for Siemens

| Topic | Behaviour | Where |
|---|---|---|
| DCP Identify filters | NameOfStation, DeviceID, DeviceVendor, IP matched; **AliasName -> never answered** (needs LLDP neighbour data). If the S7 identifies by alias (topology + "device replacement without exchangeable medium"), SoftPN stays silent. Candidate fix: answer AliasName `port-001.<neighbour chassis>` from received LLDP. | `src/pn/pn_dcp.cpp` `identifyMatches()` |
| DCP Set IP | accepted only if equal to the NIC's static IP (`DCP: refused IP ...` otherwise) | `pn_dcp.cpp` `handleSet()` |
| LLDP sent | Chassis ID subtype 7 = NameOfStation (MAC while unnamed), Port ID subtype 7 = `port-001`, PNIO delay/portstatus/chassis-MAC TLVs, 802.3 MAC/PHY with the real MAU type. **Received LLDP is not evaluated.** | `src/pn/pn_lldp.cpp` |
| Records on slot 0 | every write (PDPortDataAdjust 0x802F, boundaries, PDPortDataCheck 0x802B) accepted and ignored | `src/pn/pn_ar.cpp` `writeRecord()` |
| Reads | only I&M0 answered; an S7 may read more (I&M1-4, PDev records). Unknown until a capture shows it. | `pn_ar.cpp` |
| Update time | Connect below 8 ms refused (`DB 81 02 0B`); GSDML minimum 8 ms | `DeviceConfig::minUpdateTimeMs` |

Expect more Siemens-only gaps once the device is found (startup reads, alarms, diagnosis). Each needs a
capture: compare with the CODESYS startup in `tests/profinet/captures/codesys-sp22-startup-32ms.pcapng`.

## Next steps

1. Get the answers / capture for the 5 checks above. If DCP Identify reaches the PC but no response goes out,
   look at the Identify filter blocks in the capture (AliasName?) and `identifyMatches()`.
2. Once the AR starts: follow the console (`AR: ...` lines) and the CPU diagnostics buffer; fix what the S7 needs,
   rerun `pn_selftest`, rebuild the kit, send it to the user.
3. Afterwards: STOP/RUN, demo restart, 30 min run (as done with CODESYS), then record the result in
   `docs/phases/phase-9-profinet.md` (test results table) and the memory file.

## Rules

- After code changes: `cmake --build --preset vs2026-release` (configure preset is `vs2026`), `pn_selftest` must
  stay PASS, rerun `python tools/gen_profinet_docs.py` if a file shown in a phase doc changed.
- The GSDML file name stays `...-20261013.xml` (TIA has it installed under that name); after any change check it
  is well-formed and send it again.
- Dev.3 (`D:\Univision\Dev.3`) is on hold for PROFINET; never switch or stash its checkout.
- Parallel work in another chat: own NDIS driver replacing Npcap ([HANDOFF-softpn-ndis-driver.md](HANDOFF-softpn-ndis-driver.md),
  phases D0-D3 done or in test, VMs pndevA / pndevB on PC-PROVE). The Siemens test still uses **Npcap** on the
  colleague's PC; the `--backend` option (auto/npcap/ndis) exists in the current kit, `auto` falls back to Npcap
  when the driver is not installed.
