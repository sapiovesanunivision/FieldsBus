# SoftFieldbus EtherNet/IP: PLC setup with the EDS (CODESYS)

How to connect a CODESYS PLC (EtherNet/IP scanner) to the ProInspect process image through
**ProInspectProcessImageSoftFieldbusService**, using the EDS file. The PLC sees the same 112 / 252 bytes as with the
Hilscher card, as named structures.

Tested setup (2026-10-15):

| | |
|---|---|
| PLC | CODESYS Control Win V3 x64 (soft PLC), CODESYS V3.5 SP22 Patch 4, on PC-PROVE 10.0.0.128 |
| ProInspect PC | development PC 10.0.0.168, NIC "Ethernet", office LAN |
| PC software | `ProInspectProcessImageService145_x64.exe` + `ProInspectProcessImageSoftFieldbusService145_x64.exe` (Dev.3, Release_2026) |
| Files | `ProInspectSoftFieldbus.eds`, `ProInspectEip_PLCopen.xml` (or `ProInspectEip.st`) |

```
 PLC (scanner) 10.0.0.128                          ProInspect PC 10.0.0.168
 CODESYS project                                    ProInspectProcessImageService (shared memory, slot 0)
   EtherNet/IP Scanner                                 ^
     ProInspect SoftFieldbus (EDS)  --- O->T 112 B --> ProInspectProcessImageSoftFieldbusService -process eip
       Exclusive Owner, RPI 10 ms  <-- T->O 252 B ---    (adapter: TCP/UDP 44818, UDP 2222)
```

## 1. Files

| File | Where it comes from |
|---|---|
| `ProInspectSoftFieldbus.eds` | Dev.3 `...\UvpProcessImage\ProInspectProcessImageSoftFieldbusService\EDS\`; installed by the service setup in `System145_x64\EDS` |
| `ProInspectEip_PLCopen.xml` | FieldsBus `docs/plc/`: DUTs for the 112 / 252 bytes + test program PLC_PRG, importable |
| `ProInspectEip.st` | FieldsBus `docs/plc/`: the same code as text, for copy and paste |

Copy them next to the CODESYS project (here `E:\PLC\ProcessImage` on 10.0.0.128).

## 2. ProInspect PC

1. Nothing else may use port 44818: stop RSLinx, the Hilscher service, `eip_adapter_demo`, other SoftFieldbus instances.
2. Start the process image, then the SoftFieldbus service in a console (no installation or administrator rights needed):
   ```bat
   cd /d D:\Univision\Dev.3\output\v145\Release_2026\bin
   start ProInspectProcessImageService145_x64.exe -process
   ProInspectProcessImageSoftFieldbusService145_x64.exe -process eip
   ```
   The console prints `SoftFieldbus EtherNet/IP started on 0.0.0.0, slot 0, PLC -> PC 112 bytes, PC -> PLC 252 bytes`.
   Key `t` shows the state, `o` shows recipeId, inputs and outputs, `h` the help.
   An installed service (setup) does the same with `Transport` = `eip` in the registry.
3. Windows Firewall: allow the service executable on the network profile of the PLC NIC (Windows asks at the first
   start; the setup adds the rule "ProInspect SoftFieldbus").
4. The identity must match the EDS (registry `Parameters\eip`: VendorId 65535, ProductCode 1, revision 1.1 - the
   defaults without a registry key).

## 3. CODESYS project

1. **New project**: File -> New Project -> *Standard project*; device **CODESYS Control Win V3 x64** (or the real PLC),
   PLC_PRG in **Structured Text (ST)**.
2. **Communication**: start the soft PLC (tray icon -> *Start PLC*). Double-click *Device* -> *Communication Settings*
   -> select the PLC and press Enter (first time: create the device user).
3. **Install the EDS**: Tools -> Device Repository -> *Install...* -> `ProInspectSoftFieldbus.eds`. It appears as
   *ProInspect SoftFieldbus* (vendor Univision) under EtherNet/IP remote adapters.
4. **Ethernet**: right-click *Device* -> *Add Device* -> Fieldbuses -> Ethernet Adapter -> **Ethernet**. Double-click it,
   *Network interface* = the PLC's NIC on the ProInspect network. Leave *Adjust operating system settings* unchecked.
5. **Scanner**: right-click *Ethernet* -> *Add Device* -> **EtherNet/IP Scanner**.
6. **ProInspect device**: right-click the scanner -> *Add Device* -> **ProInspect SoftFieldbus**.
   - *General*: IP address = the ProInspect PC (**10.0.0.168**). Electronic keying can stay on: the identity matches.
   - *Connections*: the predefined **Exclusive Owner** (assemblies 150 / 100 / config 151, 112 / 252 bytes,
     point-to-point) is already there. RPI **10 ms**. With **CODESYS Control Win** set the **timeout multiplier to 16**
     (Edit Connection): the soft PLC is not real-time, and with multiplier 4 (40 ms) the PC closes the connection every
     few seconds when a packet arrives late. A real PLC works with the default 4.
   - *EtherNet/IP I/O Mapping*: note the first address of the outputs (112 bytes) and of the inputs (252 bytes),
     normally `%QB0` and `%IB0`.

## 4. PLC code

1. In a new project **delete the empty PLC_PRG** first (right-click -> Delete); otherwise the import creates
   *PLC_PRG_1*, which no task calls.
2. Select *Application* -> Project -> **Import PLCopenXML...** -> `ProInspectEip_PLCopen.xml` -> import all objects.
   It creates the DUTs `ST_PI_ViewControl`, `ST_PI_ViewStatus`, `ST_PI_ViewResult`, `ST_PI_ToPc` (112 bytes, PLC -> PC),
   `ST_PI_FromPc` (252 bytes, PC -> PLC) and `PLC_PRG`.
   Without import: create the DUTs (Add Object -> DUT -> Structure) and PLC_PRG from `ProInspectEip.st`.
3. If the I/O mapping does not start at `%QB0` / `%IB0`, change the `AT` addresses of `stToPc` / `stFromPc` in PLC_PRG.
4. *MainTask* must call `PLC_PRG` (Task Configuration -> MainTask; add the call if it is missing).

What the program does:

| Part | PLC side | Check on the PC |
|---|---|---|
| Layout | `bLayoutOk` = TRUE if the structs are 112 / 252 bytes | - |
| PC alive | `stFromPc.packetId` changes every service cycle -> `nPacketChanges` counts, `bPcAlive` = TRUE | - |
| PLC -> PC | `stToPc.inputs` counts every PLC cycle, `recipeId` = 1234, `recipe` = "SOFTFIELDBUS TEST" | service console key `o` (recipeId, inputs) |
| Trigger | every 2 s a new `viewControl[0].appTag`, `acquisitionControl` = 1 for 1 s | with ProInspect running: acquisition on view 1 |

> The trigger starts acquisitions on view 1 if ProInspect runs on that PC. Remove part 3 of the code if that is not
> wanted.

## 5. Run

1. Build -> **Generate Code** (F11): no errors.
2. Online -> **Login** (download), Debug -> **Start** (F5): *Application [run]*; scanner and device icons green.
3. Open PLC_PRG (online view, or View -> Watch -> Watch 1 with `PLC_PRG.stFromPc`, `PLC_PRG.bPcAlive`, ...):
   `bLayoutOk` TRUE, `bPcAlive` TRUE, `stFromPc.packetId` changing, `nPacketChanges` growing.
4. Service console on the PC: key `t` -> connected / run; key `o` -> recipeId 1234 and the inputs changing.
5. Let it run a few minutes: the service console (or event log) must not report "connection timed out".

## 6. Troubleshooting

| Symptom | Cause / action |
|---|---|
| Device icon red, service log "Forward_Open rejected (0x0117)" | wrong assembly instance: with the EDS connection this should not happen; with a Generic device enter 16#97 / 16#96 / 16#64 |
| "electronic key vendor/product" | EDS identity differs from `Parameters\eip`: fix the registry or turn keying off |
| "O->T size 114, expected 118" or similar | wrong size or no 32-bit run/idle header in the PLC connection |
| "T->O must be unicast (point-to-point)" | set the T->O connection type to point-to-point |
| Connection drops every few seconds ("connection timed out") | soft PLC not real-time: timeout multiplier 16 or RPI 20 ms |
| Icons green but no data | PLC_PRG not called by MainTask (PLC_PRG_1 after an import), or `AT` addresses differ from the I/O mapping |
| `bLayoutOk` FALSE | a DUT was changed: compare with the offsets in the service README ("PLC side") |
| Nothing connects | firewall on the ProInspect PC; another program on port 44818; wrong IP in *General* |
| Scanner missing / licence error on a real PLC | the PLC needs the CODESYS **EtherNet/IP Scanner** licence (the Adapter licence is not enough) |

## 7. Results

| Date | PLC | RPI / multiplier | Result |
|---|---|---|---|
| 2026-10-15 | CODESYS Control Win V3 x64 SP22 (Generic device, 32 / 32 bytes, `eip_adapter_demo`) | 10 ms / 4 | data both ways; connection timeouts every few seconds (soft PLC jitter) |
| | CODESYS Control Win V3 x64 SP22 (EDS, 112 / 252 bytes, SoftFieldbus service) | 10 ms / 16 | |
