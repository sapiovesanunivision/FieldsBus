# PROFINET test runbook (softpn PoC)

How to test the software PROFINET device against a real PROFINET controller, with the tools of this repo
only. Background: [phase-9-profinet.md](phases/phase-9-profinet.md), bring-up details:
[Phase 9.18](phases/profinet/Phase9.18-CodesysBringUp.md).

| | Test 1 | Test 2 |
|---|---|---|
| **Controller** | CODESYS Control Win SL + PROFINET controller on **10.0.0.128** | CMZ PLC with CODESYS and PROFINET controller |
| **Device** | `pn_device_demo` on the development PC (NIC "Ethernet", 10.0.0.168) | `pn_device_demo` on the PC cabled to the CMZ (dev PC or 10.0.0.128) |
| **Network** | Office LAN (same switch / subnet) | Direct cable or a dedicated switch |
| **Answers** | Gate B, Gate C, `[verify]` items, timing over an office switch | Gate D: the go / no-go timing with a real PLC |

## 0. Prepare

**Test kit** (on the development PC):
```powershell
pwsh tools\pack_profinet_test.ps1 -Build                                        # -> build\softpn-testkit
pwsh tools\pack_profinet_test.ps1 -Build -Destination \\10.0.0.128\D\SoftPN-Test  # kit for the other PC
```
It contains `pn_device_demo`, `pn_probe`, `pn_selftest`, `pn_controller_sim`, `fb_device_demo`, the C# sample with
`SoftFieldbus.Net.dll`, the C++ runtime, the GSDML and this runbook. Build is `RelWithDebInfo` (PDBs for remote
debugging, no debug runtime needed).

**Device PC** (where `pn_device_demo` runs):
- [ ] Npcap installed (free edition; lab use, at most 5 systems). "Restrict to administrators" **unchecked**.
- [ ] Firewall: allow **inbound UDP 34964** for `pn_device_demo.exe` / `fb_device_demo.exe` (Windows asks at the first
      start - allow on the network profile of the PLC NIC). DCP, RT and LLDP are Layer 2: no rule needed.
- [ ] PLC NIC properties: unbind **Microsoft LLDP Protocol Driver**. On a dedicated NIC also: power saving and
      energy-efficient Ethernet off, interrupt moderation off, static IPv4.
- [ ] Wireshark installed (for the capture in step 6).
- [ ] `pn_selftest --gsdml GSDML-V2.43-Univision-SoftPN-20261013.xml` -> `RESULT: PASS` (proves the kit runs on this PC).

**Office LAN notes (test 1):**
- DCP Identify and LLDP are Ethernet multicasts: every PC on the office segment receives them (a few frames per
  second, harmless). Cyclic data is unicast.
- The device's IP is the NIC's IP. If the dev PC gets 10.0.0.168 from DHCP, the CODESYS project must use that address,
  and must be updated if the lease changes. A DCP "Set IP" to another address is refused by the PoC (by design).
- Pick a NameOfStation nobody else uses on the LAN, e.g. `softpn-dev1`.

## 1. Gate A: timing of this PC

```bat
pn_probe --list
pn_probe --adapter "Ethernet" --jitter --period-ms 8 --seconds 60 --dst <MAC of the controller NIC>
pn_probe --adapter "Ethernet" --jitter --period-ms 4 --seconds 60 --dst <MAC of the controller NIC>
```
`--dst` sends the test frames (EtherType 0x88B5, ignored by every PROFINET device) to the controller PC only, instead
of flooding the office switch. Take the MAC from `ipconfig /all` on 10.0.0.128.

Pass: at 8 ms p99 < 8.5 ms, max < 10 ms, nothing near 24 ms. Record the numbers in the sheet (section 8).

## 2. Start the device

```bat
pn_device_demo --adapter "Ethernet" --name softpn-dev1 --in-size 32 --out-size 32
```
The demo echoes the PLC's outputs back (from byte 4 on) and counts a heartbeat in bytes 0..3; it prints statistics
every 10 s (`tx`, `rx`, `max TX period`, `late`, `ARs`, `aborts`).
Same test through the common API: `fb_device_demo --transport profinet --adapter "Ethernet" --station softpn-dev1
--in-size 32 --out-size 32`, or C#: `SoftFieldbusSample profinet "Ethernet" softpn-dev1` (64/64 bytes).

## 3. CODESYS project

1. **Device repository**: Tools -> Device Repository -> Install -> `GSDML-V2.43-Univision-SoftPN-20261013.xml`.
2. **Controller**: test 1 = "CODESYS Control Win V3 x64" on 10.0.0.128 (the PROFINET controller licence/demo must be
   active; check in the CODESYS documentation that your Control Win version supports the PROFINET controller and which
   network driver it uses). Test 2 = the CMZ device.
3. Device tree: **Ethernet** (select the controller's NIC) -> **PROFINET IO Controller** -> Add device ->
   "SoftPN device".
4. Slots: **slot 1 "Input 32 bytes (PC to PLC)"**, **slot 2 "Output 32 bytes (PLC to PC)"** - the sizes must equal
   `--in-size` / `--out-size`, otherwise the device reports the module as wrong (ModuleDiff, IOPS BAD).
5. SoftPN device -> General: **station name** `softpn-dev1`, **IP** = the device PC's NIC IP (test 1: 10.0.0.168),
   subnet mask of that NIC.
6. Send clock 1 ms, **reduction ratio 8** (update time 8 ms); keep the default watchdog (3 cycles).
7. Ethernet adapter -> **Scan for devices**: the device appears (vendor 0xFFFF, device 0x0001, "Univision SoftPN").
   Assign the name if it is not set yet. **Blink** -> the demo prints "DCP signal".
8. PLC program: map the 32 input bytes and the 32 output bytes; write a changing pattern to the outputs from byte 4;
   compare inputs[4..31] with outputs[4..31] and watch inputs[0..3] (heartbeat) count up.
9. Login, download, start.

## 4. Checks

| Gate | Check | How |
|---|---|---|
| B | Device found and named | CODESYS scan; `softpn.ini` next to the demo contains the name after a restart |
| B | Blink | demo prints "DCP signal" |
| C | Connection | demo log: `AR: connected ...`, `AR: Data`, `state: DataRun`; device green in CODESYS |
| C | Data | PLC sees the echo and the heartbeat; demo prints the outputs |
| D | 8 ms, 1 h | demo statistics: `aborts 0`, `late` small, `max TX period` < 10 ms; CODESYS log without AR loss |
| D | 16 ms and 32 ms | change the reduction ratio, download, repeat 10 min |
| D | STOP / RUN | PLC STOP -> `state: DataStop`; RUN -> `DataRun` |
| D | Cable pull | AR ends ("data-hold timer expired"); plug in -> reconnects by itself |
| D | Restart the demo | CODESYS reconnects without intervention |
| - | Refusal | reduction ratio 4 (4 ms) -> CODESYS shows a connect error; demo logs `Connect refused, PNIOStatus DB 81 02 0B` |

## 5. Optional: the process-image service

When the Dev.3 phase is done (`ProInspectProcessImageSoftFieldbusService` with `Transport=profinet`), repeat step 4
with the service instead of the demo: modules **"Output 128 bytes"** (PLC -> PC, the service uses the first 112) and
**"Input 256 bytes"** (PC -> PLC, first 252).

## 6. Wireshark capture (resolves the `[verify]` items)

On the device PC, capture the PLC NIC from before "download" until data exchange runs, plus one STOP/RUN and one
cable pull. Display filter: `pn_dcp || pn_io || pn_rt || lldp || dcerpc`.
Save the capture (`.pcapng`) next to this runbook and check the table in
[Phase 9.18](phases/profinet/Phase9.18-CodesysBringUp.md#step-9183-resolve-the-verify-items): malformed packets,
RT class value in IOCRProperties, reads CODESYS issues, RTA sequence numbers, LLDP port ID.

## 7. Remote debugging (device on 10.0.0.128)

- Install **Remote Tools for Visual Studio 2026** (x64) on 10.0.0.128 and start `msvsmon.exe`; allow it through the
  firewall as it asks.
- Build `RelWithDebInfo` (`pack_profinet_test.ps1 -Build -Destination \\10.0.0.128\D\SoftPN-Test`).
- In VS: project `pn_device_demo` -> Properties -> Debugging -> **Remote Windows Debugger**: Remote Command
  `D:\SoftPN-Test\pn_device_demo.exe`, Command Arguments `--adapter "<NIC>" --name softpn-dev1`, Working Directory
  `D:\SoftPN-Test`, Remote Server Name `10.0.0.128`. CMake regenerates the `.vcxproj`, but these settings live in the
  `.vcxproj.user` file and survive.
- **Do not debug the timing tests**: a breakpoint stops the cyclic frames and the PLC aborts the AR after 3 cycles.
  Attach only to investigate protocol problems, or set a large watchdog factor in CODESYS while debugging.

## 8. Troubleshooting

| Symptom | Likely cause / action |
|---|---|
| CODESYS scan does not find the device | NIC hardware filter drops the DCP multicast: restart the demo with `--promisc 1`. Check the right adapter (`pn_probe --list`) |
| Name assignment fails (BlockError in the demo log) | Invalid name (lowercase, digits, '-', '.'), or an AR is active |
| "refused IP ... configure the controller with the PC's static IP" | The IP in the CODESYS project differs from the NIC IP |
| Connect refused `DB 81 02 0B` | Update time below 8 ms (`--min-update-ms` lowers the limit, at your own risk) |
| "expected configuration differs (ModuleDiffBlock)" | Module sizes in CODESYS differ from `--in-size` / `--out-size` |
| `ApplicationReady sent ... (retry)` repeating, then timeout | The controller's firewall blocks UDP 34964 from the device |
| No Connect at all (demo stays WaitingForController after the scan) | Device PC firewall blocks inbound UDP 34964 |
| AR ends with "data-hold timer expired" every few seconds | Frames from the PLC are late: network (office switch load) or the controller PC; compare with `pn_probe --rx-jitter` |
| `Npcap is not installed` | Install Npcap; `wpcap.dll` must be in `C:\Windows\System32\Npcap` |

## 9. Results sheet

Copy into [phase-9-profinet.md](phases/phase-9-profinet.md) when done.

| Item | Test 1 (Control Win, office LAN) | Test 2 (CMZ) |
|---|---|---|
| Date / PCs / NIC | | |
| CODESYS version / controller | | |
| Gate A: 8 ms p99 / max | | |
| Gate A: 4 ms p99 / max | | |
| Gate B: scan, name, blink | | |
| Gate C: connected (update time) | | |
| Gate D: 1 h at 8 ms - aborts / late / max TX period | | |
| 16 ms / 32 ms | | |
| STOP/RUN, cable pull, restart | | |
| `--promisc 1` needed? | | |
| `[verify]` findings | | |
