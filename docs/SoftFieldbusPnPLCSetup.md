# SoftFieldbus PROFINET: PLC setup with the GSDML (CODESYS, TIA Portal)

How to connect a PROFINET IO-Controller (CODESYS, Siemens S7-1200 / S7-1500) to the ProInspect process image through
**ProInspectProcessImageSoftFieldbusService** with `Transport = profinet`. The PLC sees the same 112 / 252 bytes as
with the Hilscher card or EtherNet/IP, at the start of two 256-byte modules.

```
 PLC (IO-Controller)                               ProInspect PC (IO-Device)
   PROFINET IO system                                ProInspectProcessImageService (shared memory, slot 0)
     ProInspect SoftFieldbus (GSDML)                    ^
       slot 2 Output 256 B (PLC -> PC) --- RT ---->  ProInspectProcessImageSoftFieldbusService
       slot 1 Input 256 B  (PC -> PLC) <-- RT -----    Transport = profinet (raw Ethernet: softpn_ndis driver or Npcap)
```

## 1. Files

| File | Where it comes from |
|---|---|
| `GSDML-V2.43-Univision-ProInspect-<date>.xml` | Dev.3 `...\ProInspectProcessImageSoftFieldbusService\GSDML\`; installed by the setup in `System145_x64\GSDML` |
| `ProInspectPn_PLCopen.xml`, `ProInspectPn.st` | FieldsBus `docs/plc/`: CODESYS test program (import or copy / paste) |
| `ProInspectPn.scl` | FieldsBus `docs/plc/`: TIA Portal test program (external source) |

## 2. ProInspect PC

1. **Raw Ethernet**: Npcap (https://npcap.com, default options) **or** the softpn_ndis driver must be installed.
   `Parameters\profinet\L2Backend` = `auto` uses the driver if it runs, otherwise Npcap.
2. **Network card**: the NIC facing the PLC must have a **static IP** in the PLC's subnet. Set
   `Parameters\profinet\Adapter` to its name ("Ethernet 2"), MAC or IP, or set `BindAddress` to its IP.
3. **Byte order** `Parameters\profinet\ByteOrder`: **`big` for Siemens** (the default), **`little` for CODESYS**.
4. Start the process image and the service, in a console for the first test:
   ```bat
   cd /d "D:\Program Files\Univision\ProInspectProcessImageSoftFieldbusService\System145_x64"
   start ProInspectProcessImageService145_x64.exe -process
   ProInspectProcessImageSoftFieldbusService145_x64.exe -process profinet 0 big
   ```
   With the softpn_ndis driver, run the console as administrator (the installed service needs nothing extra).
   The console prints `PROFINET device on <NIC> (...)` and `state: Waiting for master`. Key `t` shows the state.
5. Firewall: the setup's rule "ProInspect SoftFieldbus" covers PROFINET RPC (UDP 34964). In a console test,
   Windows asks at the first start.

| Registry `Parameters\profinet` | Must match in the PLC project |
|---|---|
| `StationName` (default `proinspect`) | PROFINET device name |
| NIC IP (`Adapter` / `BindAddress`) | Device IP address, set in the project (the PC refuses another IP) |
| `MinUpdateTimeMs` (default 8) | Update time >= this value |
| `VendorId` 0xFFFF / `DeviceId` 2 | Given by the GSDML: do not change one without the other |

## 3. CODESYS

1. Tools -> Device Repository -> **Install** the GSDML: *ProInspect SoftFieldbus* (vendor Univision).
2. Device tree: Ethernet adapter (the PLC NIC) -> **PROFINET IO Controller** -> Add Device -> **ProInspect SoftFieldbus**.
   Slots 1 and 2 are already filled ("Input 256 bytes (PC to PLC)", "Output 256 bytes (PLC to PC)").
3. Device -> General: station name = `StationName`, IP = the PC's NIC IP, mask as the PC.
4. Update time 16 or 32 ms on CODESYS Control Win (not real time), "accepted update cycles without IO data" 10.
   A real-time CODESYS PLC can use 8 ms.
5. PLC code: delete the empty PLC_PRG, then Project -> **Import PLCopenXML** -> `ProInspectPn_PLCopen.xml`.
6. I/O mapping (no `AT` addresses): *Input_256_bytes_PC_to_PLC_* -> `Application.PLC_PRG.abFromPc`;
   *Output_256_bytes_PLC_to_PC_* -> `Application.PLC_PRG.abToPc`.
7. Build (F11), Login, Start. PC side: `ByteOrder = little`.

## 4. TIA Portal

1. Options -> Manage general station description files -> install the GSDML. Catalog: *Other field devices ->
   PROFINET IO -> Ident Systems -> Univision -> ProInspect -> ProInspect SoftFieldbus*.
2. Network view: drag the device in, **Not assigned** -> the PLC's PROFINET interface. The device view already shows
   both modules.
3. Device properties -> PROFINET interface [X1]: IP set in the project = the PC's NIC IP; device name = `StationName`
   (untick "generate automatically"); update time **16 ms** for the first test (8 ms later), accepted cycles
   without IO data **10**. Details and screenshots: [softpn-siemens-setup.md](softpn-siemens-setup.md) 4.2-4.4
   (same steps, other device).
4. PLC_1 -> External source files -> add `ProInspectPn.scl` -> Generate blocks from source.
5. OB1: call `"ProInspectPn"` with an instance DB, `hwIn` / `hwOut` = the two modules' hardware identifiers
   (System constants, `Hw_SubModule`).
6. Compile, download, RUN. PC side: `ByteOrder = big` (default).

## 5. What the program does

| Part | PLC side (CODESYS `PLC_PRG` / TIA instance DB) | Check on the PC |
|---|---|---|
| Layout | `bLayoutOk` TRUE (CODESYS: `SIZEOF`; TIA: `DPRD_DAT` / `DPWR_DAT` return 0) | - |
| PC alive | `packetId` changes -> `bPcAlive` TRUE, packet counter grows | - |
| Byte order | `bByteOrderOk` TRUE | console key `o`: recipeId **1234** |
| PLC -> PC | `inputs` counts every PLC cycle, recipe "SOFTFIELDBUS TEST" | console key `o` / `e` |
| Trigger | every 2 s a new `viewControl[0].appTag`, `acquisitionControl` = 1 for 1 s | with ProInspect running: acquisition on view 1 |

## 6. Troubleshooting

| Symptom | Cause / action |
|---|---|
| Service does not start: "Npcap is not installed ... or install and start the softpn_ndis driver" | install one of them |
| "softpn_ndis: access denied" in a console | run the console as administrator, or use `L2Backend = npcap` |
| PLC: "device not found" / name not assigned | `StationName` and the device name differ; NIC not in the PLC subnet; check with TIA "Update accessible devices" or CODESYS "Scan for devices" |
| PLC: "wrong IP" / Set IP refused | the IP in the project must be the PC's static IP: Windows owns the address |
| Connects, module error / ModuleDiff | a module was changed or the GSDML is the generic SoftPN one (DeviceID 1): use the ProInspect GSDML |
| Connection drops (watchdog) | update time too short for the PC: 16 or 32 ms, accepted cycles 10 |
| `bByteOrderOk` FALSE, recipeId 3523477504 on the PC | `ByteOrder` wrong: `big` for Siemens, `little` for CODESYS |
| `bLayoutOk` FALSE (TIA, `iRetRead` = 16#80B1) | the PI_* types were changed or the FB is optimized |
| Icons green but no data (CODESYS) | I/O mapping missing, or PLC_PRG_1 not called by the task |

## 7. Results

| Date | PLC | Backend | Update time | Byte order | Result |
|---|---|---|---|---|---|
| 2026-10-21 | CODESYS Control Win V3 x64 SP22, `ProInspectPn_PLCopen.xml`; PC side `pn_device_demo --device-id 2 --in-size 256 --out-size 256` (not the service) | Npcap | 32 ms | - | GSDML and program import OK, `bLayoutOk` TRUE, DataRun, PLC -> PC bytes as the program writes them; connection test only (the demo echoes) |
| 2026-10-21 | CODESYS Control Win V3 x64 SP22, Dev.3 service on the dev PC (10.0.0.168) | Npcap | 32 ms | little | ConnectedRun; data both ways correct |
| 2026-10-22 | CODESYS Control Win V3 x64 SP22, Dev.3 service on the test PC (10.0.0.193), settings from the registry | softpn_ndis | 32 ms | little | ConnectedRun; data both ways correct |
| | S7-1500 | | 16 ms | big | |
