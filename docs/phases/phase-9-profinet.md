# Phase 9: PROFINET RT device (softpn) - proof of concept

**Goal:** an in-house PROFINET RT IO-Device (Conformance Class A, RT_CLASS_1) on a standard Windows NIC through
Npcap, behind the same process-image API as EtherNet/IP and Modbus, to answer one question with measurements:
can a software device replace the Hilscher card for PROFINET at update times of 8-32 ms?

- Spec: [profinet-spec.md](../profinet-spec.md)
- Step-by-step phase documents: [profinet/README.md](profinet/README.md) (9.01-9.18)
- Branch: `feature/profinet-poc`

## Decisions

| Topic | Decision | Why |
|---|---|---|
| Stack | Own C++17 code (`softpn`), no p-net | p-net is GPL; same style and control as `softeip` |
| Raw Ethernet on Windows | Own NDIS protocol driver `softpn_ndis.sys` (Phase 9.20) or Npcap, both behind `L2Port`, chosen at run time | Native, no WSL; the own driver avoids the Npcap OEM licence (needs a Microsoft driver signature for products, see [HANDOFF-softpn-ndis-driver.md](../HANDOFF-softpn-ndis-driver.md)) |
| Npcap licence | Free edition for the lab (max 5 systems) | OEM redistribution licence (~$80k) is a management decision; Hilscher cards (~200 EUR) are cheaper below ~400 stations |
| Scope | CC-A, RT_CLASS_1, one AR, update time >= 8 ms | IRT needs hardware; certification out of scope |
| DCP Set IP | Accepted only if equal to the NIC's static IP | Windows owns the address; changing it needs admin rights |
| Promiscuous mode | Off by default | Stopped reception on a Hyper-V adapter; DCP multicast arrived without it |

## What exists

| Part | Files |
|---|---|
| L2 backends | `l2_port_ndis.cpp` (own driver `drivers/softpn_ndis`, Phase 9.20), `l2_port_npcap.cpp`, chosen by `l2_port_windows.cpp`; `l2_port_linux.cpp`, `l2_port_memory.cpp` |
| Protocols | DCP + persistence, LLDP, DCE/RPC CL, PNIO blocks, device model, AR state machine, cyclic RT, RTA |
| Public API | `softpn::Device` (`include/softpn/pn_device.hpp`), `softfb::Transport::ProfinetRt`, `SoftFieldbus.FieldbusTransport.ProfinetRt` |
| GSDML | `gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml` (VendorID 0xFFFF placeholder) |
| Tools | `pn_probe` (adapters, jitter, DCP), `pn_controller_sim` (minimal IO-Controller), `pn_selftest` (offline), `pn_device_demo` |

## Build results

| Build | Result |
|---|---|
| VS 2026 static, `/W4`, Debug + Release | 0 warnings |
| `SoftFieldbus.dll` + `/analyze /WX` (as Dev.3) | clean, after 2 fixes it found (LLDP shift overflow, null-pointer path in Connect) |
| .NET wrapper + C# samples | built (`/restore` needed on the command line) |
| Without the Npcap SDK | softpn builds with the softpn_ndis backend only (since Phase 9.20); `pn_selftest` 112/112 |
| Linux g++ 13 `-Wall -Wextra -Wpedantic` | 0 warnings |

## Test results

| Test | Result |
|---|---|
| `pn_selftest` (codecs, GSDML idents, end-to-end over memory cable + UDP loopback, error paths) | **107/107 PASS**: Windows Debug, Release, AddressSanitizer (3 runs), Linux |
| Linux, raw AF_PACKET over veth + netns, `pn_device_demo` vs `pn_controller_sim` | **PASS**: identify, connect, write, I&M0, PrmEnd/ApplicationReady, 373/373 echoes in 3 s, release; `--rr 4` refused `DB 81 02 0B`; skip-release -> watchdog abort |
| Windows, Npcap on the WSL virtual adapter, simulator inside WSL | DCP Identify, Connect, Write, Read I&M0, PrmEnd, ApplicationReady, ModuleDiff, IOPS BAD on wrong module: OK. Cyclic 1251/1251 with data-hold factor 10. `fb_device_demo --transport profinet` and the C# sample reach ConnectedRun with echo |
| Windows, strict PLC watchdog (data-hold factor 3 = 24 ms) | **not verifiable on this test bed**: the WSL path itself has gaps of 36 ms (sender) and 72 ms (receiver); the device's TX stayed at max 8.5-8.7 ms |
| EtherNet/IP and Modbus regression (`eip_scanner_sim`, `mb_client_test`) | PASS after the shared-header changes |
| **CODESYS PN-Controller** (Control Win V3 x64 SP22 on 10.0.0.128) vs `pn_device_demo` on 10.0.0.168 (I219-LM, Npcap, office LAN), 2026-10-15 | **DataRun**: DCP, Connect, PrmEnd, ApplicationReady (confirmed after 7 ms), cyclic 32 ms / data-hold 320 ms, device and modules green in CODESYS. Over 6 min: tx 11543 / rx 11541, max TX period 33.6 ms, late 0, aborts 0. Needed fixes 7-9 below. 2026-10-16: PLC echo + heartbeat check, STOP/RUN, demo restart and a 30 min run OK (the first `abIn[0..2] = 0x80` was a wrong `AT` address in the PLC program) |
| **CODESYS PN-Controller vs `pn_device_demo` on the own NDIS driver** (test PC, I219-LM, office LAN, no Npcap), 2026-10-19 | **DataRun**, VLAN tags on. **32 ms**, 26 min: tx 48736 / rx 48733, max TX period 35.6 ms, late 0, aborts 0. **8 ms** (data hold 80 ms), 19 min: tx 141148 / rx 141136, max TX period 11.3 ms, late 0, aborts 0 - first verification of the 8 ms target against a real controller. STOP/RUN and demo restart recover automatically (reconnect 1-4 s). PLC code: `docs/plc/SoftPN_Echo_PLCopen.xml` (no `AT %IB0`: it reads IOPS bytes) |
| **Own NDIS driver** (`softpn_ndis`, no Npcap), two Hyper-V VMs on PC-PROVE, `pn_device_demo` vs `pn_controller_sim`, 2026-10-17 | **DataRun at 8 ms**, 30 s: tx 1781 / rx 1780, max TX period 8.51 ms, late 0, aborts 0; DCP multicast received; jitter max 8.65 ms. 802.1Q-tagged frames are dropped by Hyper-V (test untagged); tagged frames, CODESYS and a physical NIC are open ([Phase 9.20](profinet/Phase9.20-NdisBackend.md)) |
| Siemens CPU 1511-1 PN (TIA Portal), 2026-10-16/17 | GSDML installs after 4 fixes TIA required ([softpn-siemens-setup.md](../softpn-siemens-setup.md) 4.1). Open: the CPU reports "IO device not found" ([HANDOFF-softpn-siemens.md](../HANDOFF-softpn-siemens.md)) |

## [verify] items against CODESYS (Control Win SP22, capture 2026-10-16)

Startup excerpt (PROFINET and RPC frames only): `tests/profinet/captures/codesys-sp22-startup-32ms.pcapng`.
Full sequence: DCP Identify (multicast) -> response 60 us -> DCP Set IP (same IP, every startup) -> Connect ->
Write -> PrmEnd -> ApplicationReady, all OK, 50 ms in total.

| Item ([Phase 9.18](profinet/Phase9.18-CodesysBringUp.md)) | Result |
|---|---|
| DCP response ServiceType / BlockError | Identify and Set responses accepted ("Set Ok", Response(Ok)) |
| IOCRProperties RT class | **2** (RT_CLASS_2 numbering for RT_CLASS_1 traffic); 1 never seen |
| FrameIDs | input CR 0x8001 chosen by CODESYS, output CR **0xFFFF = device chooses** (bug 7) |
| ARProperties | 0x40000011: **advanced startup**, ActivityTimeoutFactor 300, CMInitiatorUDPRTPort 0x8892 (bug 8) |
| ARServerBlock in the Connect response | accepted |
| Reads issued by CODESYS | **none**. One MultipleWrite with PDInterfaceAdjust (slot 0 / 0x8000), accepted |
| Frame layout | input: DAP IOPS at 0/1/2, data 3..34, IOPS 35, IOCS 36; output: IOCS 0..3, data 4..35. As in the GSDML |
| IOPS before ApplicationReady | BAD (0x00) until ApplicationReady, then GOOD (0x80); no diagnosis in CODESYS. DataStatus 0x35, CycleCounter +1024 per 32 ms |
| RTA | no alarms from the device. CODESYS sends ERR-RTA "AR consumer DHT/WDT expired" when the device stops, `CF 81 FD 11` when it drops the AR (seen around "Scan for devices"). Initial sequence numbers not exercised |
| LLDP | Chassis ID = NameOfStation, Port ID `port-001` (locally assigned), PNO Measured Delay / Port Status / Chassis MAC TLVs. CODESYS sends no LLDP. MAU type was always 100BASE-TX FD (0x10) on a 1 Gbit/s link: fixed (bug 10) |
| Promiscuous | not needed with ALL_MULTICAST (bug 9) |
| CODESYS "Scan for devices" | hangs in the IDE; no DCP frame reaches the wire. CODESYS-side; Identify All to be checked with PRONETA / TIA |
| NIC settings on 10.0.0.168 | Energy Efficient Ethernet On, Interrupt Moderation Adaptive (Phase 9.18 recommends off; not changed for this test) |

## Timing (Gate A reference, development laptop)

Device scheduling (`DeadlineTimer`, absolute due times), internal Hyper-V adapter, desktop load:

| Period | avg | p99 | max | within 0.5 ms |
|---|---|---|---|---|
| 8 ms | 8.000 ms | 8.61 ms | 11.5 ms | 97 % |
| 4 ms | 4.000 ms | 4.47 ms | 5.6 ms | 98 % |
| 1 ms | 1.005 ms | 1.58 ms | 2.6 ms | 98 % |

With the original periodic 1 ms timer the same PC reached only ~680 frames/s at 1 ms and drifted 2 % at 8 ms:
the Windows periodic timer ticked every ~1.5 ms. Repeat on the station PC and NIC for the real Gate A.

## Bugs found during the PoC (all fixed)

1. **BPF filter**: libpcap's `vlan` keyword shifts the offsets of every term after it; an EtherType test placed
   after it never matched. `vlan` is now the last term.
2. **Promiscuous mode on Hyper-V**: reception stopped after ~2.5 s. Default changed to off (option kept).
3. **Timer**: periodic 1 ms waitable timer ticks every ~1.5 ms. Cyclic thread now uses a one-shot `DeadlineTimer`.
4. **DCP**: NameOfStation persisted after the Set response (race with a controller that power-cycles). Now before.
5. **/analyze**: LLDP TLV header shift overflow; possible null pointer in Connect handling.
6. Test-only: dangling reference in the self-test rig (found with AddressSanitizer); wrong DCP header size in a test.
7. **Output IOCR FrameID** (found with CODESYS): the controller sends 0xFFFF for the output CR, meaning "the device
   chooses"; softpn refused it with `DB 81 02 09`. softpn now assigns 0x8002 and returns it in the IOCRBlockRes
   (input CR: CODESYS chose 0x8001). The accepted range now also includes the legacy 0xC000-0xFBFF. A refused
   Connect logs the IOCR FrameIDs.
8. **ApplicationReady port** (found with CODESYS): sent to `CMInitiatorUDPRTPort` (0x8892 = 34962, the RT_CLASS_UDP
   port) and never answered. Now sent to the PNIO-CM port 34964 (`DeviceConfig::controllerRpcPort`, tests only).
   The controller simulator sent its own RPC port in that field, which hid the bug; it now sends 0x8892.
9. **DCP on the physical I219-LM**: DCP Identify multicasts were not received without promiscuous mode
   (CODESYS: "DCP no RealStationName"). The Npcap port now requests the NDIS ALL_MULTICAST packet filter
   (`pcap_oid_set_request`, OID_GEN_CURRENT_PACKET_FILTER), the counterpart of the Linux multicast join.
   Verified 2026-10-16 with a Wireshark capture in non-promiscuous mode (0 foreign unicast frames in 22528):
   Identify request to 01:0e:cf:00:00:00 answered after 60 us, DataRun without `--promisc`. Promiscuous stays
   off by default and remains available as a fallback.
10. **LLDP MAU type** (found in the CODESYS capture): always 100BASE-TX FD, also on the 1 Gbit/s I219 link. New
    `L2Port::link()` (Windows `GetIfEntry2`, Linux `/sys/class/net/<if>/speed`, duplex; Windows assumes full
    duplex), queried at every LLDP frame: MAU type 10/11/15/16/29/30, advertised capability + 1000BASE-T FD on
    gigabit; unknown speed keeps 16. GSDML MAUTypeList now 16 and 30 (same file name). Verified live: MAU 0x001E,
    capability 0x6C01. `pn_selftest` 112/112 (new LLDP section).

## Open items

- [ ] Gate A on the station PC + NIC (`pn_probe --jitter`)
- [ ] Real Ethernet test with the default watchdog: two NICs / two PCs, `pn_controller_sim --dhf 3 --seconds 3600`
- [ ] First `pn_controller_sim` run on Windows (so far only on Linux / WSL); needs a NIC other than the device's
- [x] CODESYS bring-up Gate C (DataRun at 32 ms, 2026-10-15)
- [x] CODESYS: PLC-side echo + heartbeat, STOP/RUN, demo restart, 30 min run (2026-10-16)
- [ ] CODESYS Gate D (8 ms) needs a real-time controller
- [ ] CODESYS aborted the AR twice (`RTA ERR CF 81 FD 11`, 2026-10-16 08:05 / 08:06, reconnect within seconds), probably during "Scan for devices" (scan hung in the IDE): to confirm
- [x] `[verify]` list in [Phase 9.18](profinet/Phase9.18-CodesysBringUp.md) for CODESYS (see the table above); repeat with Siemens
- [x] LLDP MAU type follows the real link (bug 10)
- [ ] Identify All (PRONETA / TIA "Update accessible devices"); RTA initial sequence numbers (needs a device alarm or an abort from our side)
- [ ] CODESYS: cable pull, wrong module size, wrong IP, 16 / 8 ms, 1 h soak, `fb_device_demo` / C# sample, 256/128-byte modules
- [x] Promiscuous mode not needed on the I219-LM with ALL_MULTICAST (bug 9); check the station NIC
- [ ] Siemens S7-1200/1500 test by a colleague: [softpn-siemens-setup.md](../softpn-siemens-setup.md) (in the test kit)
- [ ] GSDML through the PI GSDML Checker
- [ ] Test 1 (CODESYS Control Win SL on 10.0.0.128, office LAN) and test 2 (CMZ PLC): [profinet-test-runbook.md](../profinet-test-runbook.md), kit `tools/pack_profinet_test.ps1`
- [ ] Dev.3 process-image service with `Transport=profinet`: prepared in [Phase 9.19](profinet/Phase9.19-Dev3ProcessImageService.md), to apply when Dev.3 is free (`sync-to-dev3.ps1` already mirrors softpn)
- [ ] README / PLAN / HANDOFF sections for PROFINET
- [ ] Management: Npcap OEM vs Hilscher vs NDISPROT driver; PI vendor ID if the product goes ahead
