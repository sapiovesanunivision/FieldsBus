# Phase 10d: TIA PLC program and the PLC setup guide

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repository: **FieldsBus**,
> branch `feature/profinet-poc`. Status: planned, not tested on a PLC.

## Goal
The Siemens S7-1200 / S7-1500 version of the test program (`ProInspectPn.scl`), plus one setup guide for CODESYS and
TIA (`SoftFieldbusPnPLCSetup.md`). Siemens is the reason the PROFINET byte order defaults to `big`: with it, the
program reads INT, DWORD and REAL directly.

## Documentation References
- [SoftPN_Echo.scl](../../plc/SoftPN_Echo.scl): `DPRD_DAT` / `DPWR_DAT` with `HW_IO`, TON latch for S7-1500
- [softpn-siemens-setup.md](../../softpn-siemens-setup.md): TIA steps for the generic SoftPN device (sections 4.1-4.4 apply)
- [SoftFieldbusEipPLCSetup.md](../../SoftFieldbusEipPLCSetup.md): the EtherNet/IP guide this one mirrors
- Siemens online help: `DPRD_DAT`, `DPWR_DAT`, `Strg_TO_Chars`

---

## Step 10d.1: TIA source file

**File:** `docs/plc/ProInspectPn.scl`

### Purpose
One external source creates the PLC data types and the FB. `DPRD_DAT` / `DPWR_DAT` move each 256-byte module in
one consistent block straight into a struct. If the struct is not exactly 256 bytes, the call fails with `W#16#80B1`,
so the return codes also check the layout.

### Implementation

```pascal
// ProInspect process image over PROFINET - test program for S7-1200 / S7-1500 (TIA Portal).
// Device: "ProInspect SoftFieldbus" from GSDML-V2.43-Univision-ProInspect-*.xml.
//   slot 1 "Input 256 bytes (PC to PLC)"  = PI_FromPc (252 bytes) + 4 bytes 0
//   slot 2 "Output 256 bytes (PLC to PC)" = PI_ToPc (112 bytes) + 144 bytes unused
// PC side: ProInspectProcessImageSoftFieldbusService, Transport = profinet, profinet\ByteOrder = big (the default):
// WORD / INT / DWORD / REAL arrive in S7 byte order, no SWAP needed. bByteOrderOk shows it.
//
// Import: PLC_1 -> External source files -> Add new external file -> this file,
//         right-click it -> Generate blocks from source. Creates the PLC data types PI_* and FB "ProInspectPn".
// Call it in OB1 with an instance DB and the hardware identifiers of the two modules
// (PLC tags -> Show all tags -> System constants, data type Hw_SubModule), for example:
//
//     "ProInspectPn_DB"(hwIn  := "ProInspect_SoftFieldbus~Input_256_bytes_(PC_to_PLC)_1",
//                       hwOut := "ProInspect_SoftFieldbus~Output_256_bytes_(PLC_to_PC)_1");
//
// Layout = ProInspectProcessImageHilscherData.h. The FB uses standard (not optimized) access, so the structs have
// the same offsets as on the PC (every member is naturally aligned). Procedure: SoftFieldbusPnPLCSetup.md.

TYPE "PI_ViewControl"
VERSION : 0.1
   STRUCT
      appTag : Word;               // partId (bits 0..11) + process (bits 12..15) if CompleteAppTag = 1
      acquisitionControl : Byte;   // a change to a non-zero value triggers an acquisition
      inspectionControl : Byte;
   END_STRUCT;
END_TYPE

TYPE "PI_ViewStatus"
VERSION : 0.1
   STRUCT
      appTag : Word;
      acquisition : Byte;
      inspection : Byte;
   END_STRUCT;
END_TYPE

TYPE "PI_ViewResult"
VERSION : 0.1
   STRUCT
      appTag : Word;
      result : Byte;
      acquisition : Byte;
   END_STRUCT;
END_TYPE

TYPE "PI_ToPc"   // PLC -> PC, 112 bytes
VERSION : 0.1
   STRUCT
      viewControl : Array[0..7] of "PI_ViewControl";   // offset 0
      recipe : Array[0..31] of Byte;                   // offset 32, recipe name, zero terminated
      recipeId : DWord;                                // offset 64
      inputs : DWord;                                  // offset 68, I/O lines of UvcIOProcessImage
      handled : Array[0..15] of Word;                  // offset 72, ended part ids
      externalAppTag : Array[0..3] of Word;            // offset 104
   END_STRUCT;
END_TYPE

TYPE "PI_FromPc"   // PC -> PLC, 252 bytes
VERSION : 0.1
   STRUCT
      packetId : Int;                                  // offset 0, increases every service cycle: heartbeat
      systemStatus : Int;                              // offset 2, one bit per view
      recipe : Array[0..31] of Byte;                   // offset 4, current recipe name
      viewStatus : Array[0..7] of "PI_ViewStatus";     // offset 36
      viewResult : Array[0..7] of "PI_ViewResult";     // offset 68
      rejectCode : Array[0..15] of Byte;               // offset 100, OR of all views
      measurementsStatus : DWord;                      // offset 116, one bit per measurement
      measurements : Array[0..31] of Real;             // offset 120
      outputs : DWord;                                 // offset 248, I/O lines of UvcIOProcessImage
   END_STRUCT;
END_TYPE

TYPE "PI_FromPc256"   // module "Input 256 bytes (PC to PLC)"
VERSION : 0.1
   STRUCT
      data : "PI_FromPc";
      pad : Array[0..3] of Byte;
   END_STRUCT;
END_TYPE

TYPE "PI_ToPc256"   // module "Output 256 bytes (PLC to PC)"
VERSION : 0.1
   STRUCT
      data : "PI_ToPc";
      pad : Array[0..143] of Byte;
   END_STRUCT;
END_TYPE

FUNCTION_BLOCK "ProInspectPn"
{ S7_Optimized_Access := 'FALSE' }
VERSION : 0.1
   VAR_INPUT
      hwIn  : HW_IO;     // hardware identifier of "Input 256 bytes (PC to PLC)", slot 1
      hwOut : HW_IO;     // hardware identifier of "Output 256 bytes (PLC to PC)", slot 2
   END_VAR

   VAR
      stFromPc : "PI_FromPc256";
      stToPc : "PI_ToPc256";
      iRetRead : Int;          // 0 = OK; W#16#80B1 = the struct is not 256 bytes
      iRetWrite : Int;         // 0 = OK
      bLayoutOk : Bool;        // TRUE = both modules read / written without error
      iLastPacketId : Int;
      diStep : DInt;           // packetId change since the last change seen
      bByteOrderOk : Bool;     // TRUE = packetId grows by 1..255 per change: the service sends big-endian
      udPacketChanges : UDInt; // counts while the PC is alive
      bPacketChanged : Bool;
      tPcSilent : TON_TIME;
      bPcAlive : Bool;         // packetId changed during the last 500 ms
      tTrigger : TON_TIME;     // test trigger on view 1 every 2 s
      bTriggerTick : Bool;     // tTrigger.Q latched: TRUE for one cycle every 2 s
      uiTrigger : UInt;
      sRecipe : String[31] := 'SOFTFIELDBUS TEST';
      uiChars : UInt;
      i : Int;
   END_VAR

BEGIN
    // 1. consistent read of the whole input module (PC -> PLC)
    #iRetRead := DPRD_DAT(LADDR := #hwIn, RECORD => #stFromPc);

    // 2. heartbeat: packetId changes every service cycle (Msec, default 10 ms)
    #bPacketChanged := #stFromPc.data.packetId <> #iLastPacketId;
    IF #bPacketChanged THEN
        // big-endian (right): +1..+255 per change; little-endian (wrong): +256 or more
        #diStep := INT_TO_DINT(#stFromPc.data.packetId) - INT_TO_DINT(#iLastPacketId);
        IF #diStep < 0 THEN
            #diStep := #diStep + 65536;
        END_IF;
        #bByteOrderOk := #diStep < 256;
        #iLastPacketId := #stFromPc.data.packetId;
        #udPacketChanges := #udPacketChanges + 1;
    END_IF;
    #tPcSilent(IN := NOT #bPacketChanged, PT := T#500MS);   // restarts on every change
    #bPcAlive := NOT #tPcSilent.Q;

    // 3. PLC -> PC: values visible in the service console (key o: recipeId, inputs, outputs)
    #stToPc.data.inputs := UDINT_TO_DWORD(DWORD_TO_UDINT(#stToPc.data.inputs) + 1);   // changes every PLC cycle
    #stToPc.data.recipeId := DINT_TO_DWORD(1234);   // the console shows 1234 only if the byte order matches
    FOR #i := 0 TO 31 DO
        #stToPc.data.recipe[#i] := 16#00;
    END_FOR;
    Strg_TO_Chars(Strg := #sRecipe, pChars := 0, Cnt => #uiChars, Chars := #stToPc.data.recipe);

    // 4. test trigger on view 1: new appTag and acquisitionControl = 1 for the first second of every 2 s.
    //    Self-resetting timer: latch Q in a Bool (as SoftPN_Echo.scl 0.2; "IN := NOT #t.Q" never fires on S7-1500).
    #tTrigger(IN := NOT #bTriggerTick, PT := T#2S);
    #bTriggerTick := #tTrigger.Q;
    IF #bTriggerTick THEN
        #uiTrigger := #uiTrigger + 1;
        #stToPc.data.viewControl[0].appTag := UINT_TO_WORD(#uiTrigger) AND 16#0FFF;
    END_IF;
    IF #tTrigger.ET < T#1S THEN
        #stToPc.data.viewControl[0].acquisitionControl := 16#01;
    ELSE
        #stToPc.data.viewControl[0].acquisitionControl := 16#00;
    END_IF;

    // 5. consistent write of the whole output module (PLC -> PC); pad stays 0
    #iRetWrite := DPWR_DAT(LADDR := #hwOut, RECORD := #stToPc);

    #bLayoutOk := #iRetRead = 0 AND #iRetWrite = 0;
END_FUNCTION_BLOCK
```

### Key Concepts
1. **`S7_Optimized_Access := 'FALSE'`**: in an optimized block TIA reorders members and adds padding, so the offsets
   would no longer match the PC. Standard access keeps the declared order. WORD / DWORD / REAL start at even
   offsets, and every offset here is a multiple of 4 anyway.
2. **`bLayoutOk` from the return codes**: S7 SCL has no `SIZEOF`. `DPRD_DAT` refuses a `RECORD` whose length differs
   from the module (`W#16#80B1`), which is the same check.
3. **Byte-order check mirrored**: here *big* is right. With `ByteOrder=little` on the PC, `bByteOrderOk` goes FALSE
   and the console shows recipeId 3523477504.
4. **To verify on the CPU** (not tested yet): that `DPRD_DAT` accepts 256 bytes on the target CPU (S7-1500: yes per
   the manual; S7-1200: check the limit of the firmware). As a fallback, use tags of type `"PI_FromPc256"` /
   `"PI_ToPc256"` at the module's I/Q start addresses. Also check that `Strg_TO_Chars` accepts `Array of Byte`
   (S7-1200 from firmware 4.2).

---

## Step 10d.2: PLC setup guide

**File:** `docs/SoftFieldbusPnPLCSetup.md`

### Purpose
The PROFINET counterpart of `SoftFieldbusEipPLCSetup.md`: one page for the commissioning engineer, from the PC
settings to "data running", for CODESYS and TIA. The GSDML README in Dev.3 (10h) links to it.

### Implementation

````markdown
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
| | CODESYS Control Win V3 x64 SP22 | Npcap | 16 ms | little | |
| | CODESYS Control Win V3 x64 SP22 | softpn_ndis | 16 ms | little | |
| | S7-1500 | | 16 ms | big | |
````

### Key Concepts
1. **One guide for both PLC brands**: the PC side is identical. Only the byte order and the engineering steps differ.
2. **The guide states the matching rules** (station name, IP, update time, identity), because they are the usual
   first-connection errors seen in Phase 9.

---

## Verification Checklist

- [ ] TIA: "Generate blocks from source" creates 7 PLC data types and FB `ProInspectPn` without errors
- [ ] TIA: compile with 0 errors on an S7-1500 (and an S7-1200 if available)
- [ ] `SoftFieldbusPnPLCSetup.md` links resolve
- [ ] 10j: S7 with `ByteOrder=big`: `bLayoutOk`, `bPcAlive`, `bByteOrderOk` TRUE; console recipeId 1234

---

## Next Phase
Continue to [Phase10e-SyncAndDllProject.md](Phase10e-SyncAndDllProject.md): sync and DLL project in Dev.3.
