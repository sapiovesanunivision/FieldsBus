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
| Raw Ethernet on Windows | Npcap behind `L2Port` | Native, no WSL; an NDISPROT-based backend stays possible |
| Npcap licence | Free edition for the lab (max 5 systems) | OEM redistribution licence (~$80k) is a management decision; Hilscher cards (~200 EUR) are cheaper below ~400 stations |
| Scope | CC-A, RT_CLASS_1, one AR, update time >= 8 ms | IRT needs hardware; certification out of scope |
| DCP Set IP | Accepted only if equal to the NIC's static IP | Windows owns the address; changing it needs admin rights |
| Promiscuous mode | Off by default | Stopped reception on a Hyper-V adapter; DCP multicast arrived without it |

## What exists

| Part | Files |
|---|---|
| L2 backends | `l2_port_npcap.cpp`, `l2_port_linux.cpp`, `l2_port_memory.cpp` |
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
| Without the Npcap SDK | everything else builds; PROFINET reports "no PROFINET support" |
| Linux g++ 13 `-Wall -Wextra -Wpedantic` | 0 warnings |

## Test results

| Test | Result |
|---|---|
| `pn_selftest` (codecs, GSDML idents, end-to-end over memory cable + UDP loopback, error paths) | **107/107 PASS**: Windows Debug, Release, AddressSanitizer (3 runs), Linux |
| Linux, raw AF_PACKET over veth + netns, `pn_device_demo` vs `pn_controller_sim` | **PASS**: identify, connect, write, I&M0, PrmEnd/ApplicationReady, 373/373 echoes in 3 s, release; `--rr 4` refused `DB 81 02 0B`; skip-release -> watchdog abort |
| Windows, Npcap on the WSL virtual adapter, simulator inside WSL | DCP Identify, Connect, Write, Read I&M0, PrmEnd, ApplicationReady, ModuleDiff, IOPS BAD on wrong module: OK. Cyclic 1251/1251 with data-hold factor 10. `fb_device_demo --transport profinet` and the C# sample reach ConnectedRun with echo |
| Windows, strict PLC watchdog (data-hold factor 3 = 24 ms) | **not verifiable on this test bed**: the WSL path itself has gaps of 36 ms (sender) and 72 ms (receiver); the device's TX stayed at max 8.5-8.7 ms |
| EtherNet/IP and Modbus regression (`eip_scanner_sim`, `mb_client_test`) | PASS after the shared-header changes |

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

## Open items

- [ ] Gate A on the station PC + NIC (`pn_probe --jitter`)
- [ ] Real Ethernet test with the default watchdog: two NICs / two PCs, `pn_controller_sim --dhf 3 --seconds 3600`
- [ ] First `pn_controller_sim` run on Windows (so far only on Linux / WSL); needs a NIC other than the device's
- [ ] CODESYS bring-up (Gates C and D) and the `[verify]` list in [Phase 9.18](profinet/Phase9.18-CodesysBringUp.md)
- [ ] Promiscuous mode needed on the station NIC?
- [ ] GSDML through the PI GSDML Checker
- [ ] README / PLAN / HANDOFF sections for PROFINET
- [ ] Management: Npcap OEM vs Hilscher vs NDISPROT driver; PI vendor ID if the product goes ahead
