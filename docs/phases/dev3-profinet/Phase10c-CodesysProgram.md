# Phase 10c: CODESYS PLC program

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repository: **FieldsBus**,
> branch `feature/profinet-poc`. Status: planned, not tested on a PLC.

## Goal
The PROFINET counterpart of `docs/plc/ProInspectEip.st` / `ProInspectEip_PLCopen.xml`: the same DUTs and test logic,
reading and writing the two 256-byte modules of the ProInspect GSDML. It also adds a byte-order check, because
CODESYS needs `profinet\ByteOrder=little` while the service default is `big`.

## Documentation References
- [ProInspectEip.st](../../plc/ProInspectEip.st), [ProInspectEip_PLCopen.xml](../../plc/ProInspectEip_PLCopen.xml): the EtherNet/IP version
- [SoftPN_Echo_PLCopen.xml](../../plc/SoftPN_Echo_PLCopen.xml): I/O mapping without `AT` on PROFINET
- [SoftFieldbusEipPLCSetup.md](../../SoftFieldbusEipPLCSetup.md): CODESYS steps for the EtherNet/IP version

---

## Step 10c.1: Copy / paste version

**File:** `docs/plc/ProInspectPn.st`

### Purpose
A text version for PLCs and CODESYS versions where the PLCopen import does not work. The DUTs are those of
`ProInspectEip.st`, because the layout is the same `ProInspectProcessImageHilscherData.h`.

### Implementation

```iecst
(* ProInspect process image over PROFINET - CODESYS test code (copy / paste version)
   Device: "ProInspect SoftFieldbus" from GSDML-V2.43-Univision-ProInspect-*.xml, under a PROFINET IO Controller.
   Slot 1 "Input 256 bytes (PC to PLC)", slot 2 "Output 256 bytes (PLC to PC)" (both inserted by the GSDML).
   PC side: ProInspectProcessImageSoftFieldbusService -process profinet 0 little
            (or registry Parameters\profinet\ByteOrder = "little").
   Same code as ProInspectPn_PLCopen.xml (Project -> Import PLCopenXML). Procedure: SoftFieldbusPnPLCSetup.md.

   abFromPc / abToPc have NO AT address: map them in the I/O mapping of the two modules (PROFINET module data do
   not start at %IB0 / %QB0, the first bytes are IOPS status):
     Input_256_bytes_PC_to_PLC_  -> PNIO Module I/O Mapping -> Variable: Application.PLC_PRG.abFromPc
     Output_256_bytes_PLC_to_PC_ -> PNIO Module I/O Mapping -> Variable: Application.PLC_PRG.abToPc

   Layout = ProInspectProcessImageHilscherData.h (#pragma pack 4), at offset 0 of each module; the rest is zero.
   Byte order: CODESYS (x86 / ARM) is little-endian -> the service must send "little". bByteOrderOk shows it.

   Create each DUT with Application -> Add Object -> DUT -> Structure, paste the TYPE ... END_TYPE text. *)

(* ===================== DUT ST_PI_ViewControl (4 bytes) ===================== *)
TYPE ST_PI_ViewControl :
STRUCT
    appTag             : WORD;    // partId (bits 0..11) + process (bits 12..15) if CompleteAppTag = 1
    acquisitionControl : BYTE;    // a change to a non-zero value triggers an acquisition
    inspectionControl  : BYTE;
END_STRUCT
END_TYPE

(* ===================== DUT ST_PI_ViewStatus (4 bytes) ===================== *)
TYPE ST_PI_ViewStatus :
STRUCT
    appTag      : WORD;
    acquisition : BYTE;
    inspection  : BYTE;
END_STRUCT
END_TYPE

(* ===================== DUT ST_PI_ViewResult (4 bytes) ===================== *)
TYPE ST_PI_ViewResult :
STRUCT
    appTag      : WORD;
    result      : BYTE;
    acquisition : BYTE;
END_STRUCT
END_TYPE

(* ===================== DUT ST_PI_ToPc: PLC -> PC, 112 bytes (module "Output 256 bytes", bytes 0..111) ===================== *)
TYPE ST_PI_ToPc :
STRUCT
    viewControl    : ARRAY[0..7] OF ST_PI_ViewControl;  // offset 0   (8 x 4)
    recipe         : ARRAY[0..31] OF BYTE;               // offset 32  recipe name, zero terminated
    recipeId       : DWORD;                              // offset 64
    inputs         : DWORD;                              // offset 68  I/O lines of UvcIOProcessImage
    handled        : ARRAY[0..15] OF WORD;               // offset 72  ended part ids
    externalAppTag : ARRAY[0..3] OF WORD;                // offset 104
END_STRUCT
END_TYPE

(* ===================== DUT ST_PI_FromPc: PC -> PLC, 252 bytes (module "Input 256 bytes", bytes 0..251) ===================== *)
TYPE ST_PI_FromPc :
STRUCT
    packetId           : INT;                            // offset 0   increases every service cycle: heartbeat
    systemStatus       : INT;                            // offset 2   one bit per view
    recipe             : ARRAY[0..31] OF BYTE;           // offset 4   current recipe name
    viewStatus         : ARRAY[0..7] OF ST_PI_ViewStatus;// offset 36
    viewResult         : ARRAY[0..7] OF ST_PI_ViewResult;// offset 68
    rejectCode         : ARRAY[0..15] OF BYTE;           // offset 100 OR of all views
    measurementsStatus : DWORD;                          // offset 116 one bit per measurement
    measurements       : ARRAY[0..31] OF REAL;           // offset 120
    outputs            : DWORD;                          // offset 248 I/O lines of UvcIOProcessImage
END_STRUCT
END_TYPE

(* ===================== PLC_PRG: DECLARATION ===================== *)
PROGRAM PLC_PRG
VAR
    // module data: map in the I/O mapping of the modules, no AT (see the header)
    abFromPc : ARRAY[0..255] OF BYTE;  // slot 1 "Input 256 bytes (PC to PLC)"
    abToPc   : ARRAY[0..255] OF BYTE;  // slot 2 "Output 256 bytes (PLC to PC)", bytes 112..255 stay 0

    stFromPc : ST_PI_FromPc;           // PC -> PLC, 252 bytes, copied from abFromPc every cycle
    stToPc   : ST_PI_ToPc;             // PLC -> PC, 112 bytes, copied to abToPc every cycle
    pByte    : POINTER TO BYTE;

    bLayoutOk      : BOOL;             // TRUE = the structs are exactly 112 / 252 bytes
    iLastPacketId  : INT;
    diStep         : DINT;             // packetId change since the last change seen
    bByteOrderOk   : BOOL;             // TRUE = packetId grows by 1..255 per change: the service sends little-endian
    nPacketChanges : UDINT;            // counts while the PC is alive
    bPacketChanged : BOOL;
    tPcSilent      : TON;
    bPcAlive       : BOOL;             // packetId changed during the last 500 ms

    tTrigger       : TON;              // test trigger on view 1 every 2 s
    nTrigger       : UINT;
    sRecipe        : STRING(31) := 'SOFTFIELDBUS TEST';
    i              : INT;
END_VAR

(* ===================== PLC_PRG: CODE ===================== *)
// 0. layout check: must be TRUE, otherwise the structs do not match the 112 / 252 bytes
bLayoutOk := SIZEOF(ST_PI_ToPc) = 112 AND SIZEOF(ST_PI_FromPc) = 252;

// 1. module -> struct: bytes 0..251 of the input module
pByte := ADR(stFromPc);
FOR i := 0 TO 251 DO
    pByte^ := abFromPc[i];             // CODESYS dereferences only a variable, not (p + i)^
    pByte := pByte + 1;
END_FOR

// 2. PC -> PLC: packetId changes every service cycle (Msec, default 10 ms)
bPacketChanged := stFromPc.packetId <> iLastPacketId;
IF bPacketChanged THEN
    // little-endian (right): +1..+255 per change; big-endian (wrong): +256 or more
    diStep := INT_TO_DINT(stFromPc.packetId) - INT_TO_DINT(iLastPacketId);
    IF diStep < 0 THEN
        diStep := diStep + 65536;
    END_IF
    bByteOrderOk := diStep < 256;
    iLastPacketId := stFromPc.packetId;
    nPacketChanges := nPacketChanges + 1;
END_IF
tPcSilent(IN := NOT bPacketChanged, PT := T#500MS);   // restarts on every change
bPcAlive := NOT tPcSilent.Q;

// 3. PLC -> PC: values visible in the service console (key o: recipeId, inputs, outputs)
stToPc.inputs := stToPc.inputs + 1;    // changes every PLC cycle
stToPc.recipeId := 1234;               // the console shows 1234 only if the byte order matches
pByte := ADR(sRecipe);                 // recipe name: STRING(31) = 32 bytes incl. the terminating 0
FOR i := 0 TO 31 DO
    stToPc.recipe[i] := pByte^;
    pByte := pByte + 1;
END_FOR

// 4. test trigger on view 1: new appTag and acquisitionControl = 1 for the first second of every 2 s
tTrigger(IN := NOT tTrigger.Q, PT := T#2S);
IF tTrigger.Q THEN
    nTrigger := nTrigger + 1;
    stToPc.viewControl[0].appTag := UINT_TO_WORD(nTrigger AND 16#0FFF);
END_IF
IF tTrigger.ET < T#1S THEN
    stToPc.viewControl[0].acquisitionControl := 1;
ELSE
    stToPc.viewControl[0].acquisitionControl := 0;
END_IF

// 5. struct -> module: bytes 0..111 of the output module (112..255 are never written: 0)
pByte := ADR(stToPc);
FOR i := 0 TO 111 DO
    abToPc[i] := pByte^;
    pByte := pByte + 1;
END_FOR
```

### Key Concepts
1. **Byte copy instead of `MEMCPY`**: this way no library (SysMem, MemoryUtils) is needed. It is the same
   pointer pattern as the recipe copy in the EtherNet/IP program.
2. **`bByteOrderOk`**: `packetId` grows by 1 per service cycle. Read with the wrong byte order, each step shows up as
   256 or more. This check is only meaningful after a few changes. It stays FALSE until the first change.
3. **`tPcSilent(IN := NOT bPacketChanged)`**: one timer call per cycle. A change restarts the 500 ms timer.

---

## Step 10c.2: PLCopen XML version

**File:** `docs/plc/ProInspectPn_PLCopen.xml`

### Purpose
Import in one go: Project -> Import PLCopenXML creates the five DUTs and `PLC_PRG`.

### Implementation

```xml
<?xml version="1.0" encoding="utf-8"?>
<!-- ProInspect process image over PROFINET - CODESYS test code (256-byte modules of the ProInspect GSDML).
     Import: select "Application" in the device tree, Project -> Import PLCopenXML..., this file.
     Creates the DUTs ST_PI_ViewControl, ST_PI_ViewStatus, ST_PI_ViewResult, ST_PI_ToPc (112 bytes, PLC -> PC),
     ST_PI_FromPc (252 bytes, PC -> PLC) and the program PLC_PRG. Delete the empty PLC_PRG of a new project first,
     otherwise CODESYS imports this one as PLC_PRG_1 (then let MainTask call PLC_PRG_1).
     abFromPc / abToPc have NO AT address: map them in the I/O mapping (PNIO Module I/O Mapping -> Variable):
       Input_256_bytes_PC_to_PLC_  -> Application.PLC_PRG.abFromPc
       Output_256_bytes_PLC_to_PC_ -> Application.PLC_PRG.abToPc
     PC side: Parameters\profinet\ByteOrder = "little" (CODESYS is little-endian); bByteOrderOk shows it.
     Procedure: SoftFieldbusPnPLCSetup.md. Same code as ProInspectPn.st. -->
<project xmlns="http://www.plcopen.org/xml/tc6_0200">
  <fileHeader companyName="Univision" productName="CODESYS" productVersion="CODESYS V3.5 SP22 Patch 4" creationDateTime="2026-10-21T12:00:00" />
  <contentHeader name="ProcessImagePn" modificationDateTime="2026-10-21T12:00:00">
    <coordinateInfo>
      <fbd><scaling x="1" y="1" /></fbd>
      <ld><scaling x="1" y="1" /></ld>
      <sfc><scaling x="1" y="1" /></sfc>
    </coordinateInfo>
  </contentHeader>
  <types>
    <dataTypes>
      <dataType name="ST_PI_ViewControl">
        <baseType>
          <struct>
            <variable name="appTag">
              <type><WORD /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">partId (bits 0..11) + process (bits 12..15) if CompleteAppTag = 1</xhtml></documentation>
            </variable>
            <variable name="acquisitionControl">
              <type><BYTE /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">a change to a non-zero value triggers an acquisition</xhtml></documentation>
            </variable>
            <variable name="inspectionControl"><type><BYTE /></type></variable>
          </struct>
        </baseType>
      </dataType>
      <dataType name="ST_PI_ViewStatus">
        <baseType>
          <struct>
            <variable name="appTag"><type><WORD /></type></variable>
            <variable name="acquisition"><type><BYTE /></type></variable>
            <variable name="inspection"><type><BYTE /></type></variable>
          </struct>
        </baseType>
      </dataType>
      <dataType name="ST_PI_ViewResult">
        <baseType>
          <struct>
            <variable name="appTag"><type><WORD /></type></variable>
            <variable name="result"><type><BYTE /></type></variable>
            <variable name="acquisition"><type><BYTE /></type></variable>
          </struct>
        </baseType>
      </dataType>
      <dataType name="ST_PI_ToPc">
        <baseType>
          <struct>
            <variable name="viewControl">
              <type><array><dimension lower="0" upper="7" /><baseType><derived name="ST_PI_ViewControl" /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 0 (8 x 4)</xhtml></documentation>
            </variable>
            <variable name="recipe">
              <type><array><dimension lower="0" upper="31" /><baseType><BYTE /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 32: recipe name, zero terminated</xhtml></documentation>
            </variable>
            <variable name="recipeId">
              <type><DWORD /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 64</xhtml></documentation>
            </variable>
            <variable name="inputs">
              <type><DWORD /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 68: I/O lines of UvcIOProcessImage</xhtml></documentation>
            </variable>
            <variable name="handled">
              <type><array><dimension lower="0" upper="15" /><baseType><WORD /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 72: ended part ids</xhtml></documentation>
            </variable>
            <variable name="externalAppTag">
              <type><array><dimension lower="0" upper="3" /><baseType><WORD /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 104</xhtml></documentation>
            </variable>
          </struct>
        </baseType>
      </dataType>
      <dataType name="ST_PI_FromPc">
        <baseType>
          <struct>
            <variable name="packetId">
              <type><INT /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 0: increases every service cycle (heartbeat)</xhtml></documentation>
            </variable>
            <variable name="systemStatus">
              <type><INT /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 2: one bit per view</xhtml></documentation>
            </variable>
            <variable name="recipe">
              <type><array><dimension lower="0" upper="31" /><baseType><BYTE /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 4: current recipe name</xhtml></documentation>
            </variable>
            <variable name="viewStatus">
              <type><array><dimension lower="0" upper="7" /><baseType><derived name="ST_PI_ViewStatus" /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 36</xhtml></documentation>
            </variable>
            <variable name="viewResult">
              <type><array><dimension lower="0" upper="7" /><baseType><derived name="ST_PI_ViewResult" /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 68</xhtml></documentation>
            </variable>
            <variable name="rejectCode">
              <type><array><dimension lower="0" upper="15" /><baseType><BYTE /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 100: OR of all views</xhtml></documentation>
            </variable>
            <variable name="measurementsStatus">
              <type><DWORD /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 116: one bit per measurement</xhtml></documentation>
            </variable>
            <variable name="measurements">
              <type><array><dimension lower="0" upper="31" /><baseType><REAL /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 120</xhtml></documentation>
            </variable>
            <variable name="outputs">
              <type><DWORD /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">offset 248: I/O lines of UvcIOProcessImage</xhtml></documentation>
            </variable>
          </struct>
        </baseType>
      </dataType>
    </dataTypes>
    <pous>
      <pou name="PLC_PRG" pouType="program">
        <interface>
          <localVars>
            <variable name="abFromPc">
              <type><array><dimension lower="0" upper="255" /><baseType><BYTE /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">slot 1 "Input 256 bytes (PC to PLC)": map in the module's I/O mapping, no AT</xhtml></documentation>
            </variable>
            <variable name="abToPc">
              <type><array><dimension lower="0" upper="255" /><baseType><BYTE /></baseType></array></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">slot 2 "Output 256 bytes (PLC to PC)": map in the module's I/O mapping, no AT; bytes 112..255 stay 0</xhtml></documentation>
            </variable>
            <variable name="stFromPc">
              <type><derived name="ST_PI_FromPc" /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">PC -&gt; PLC, 252 bytes, copied from abFromPc every cycle</xhtml></documentation>
            </variable>
            <variable name="stToPc">
              <type><derived name="ST_PI_ToPc" /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">PLC -&gt; PC, 112 bytes, copied to abToPc every cycle</xhtml></documentation>
            </variable>
            <variable name="pByte"><type><pointer><baseType><BYTE /></baseType></pointer></type></variable>
            <variable name="bLayoutOk">
              <type><BOOL /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">TRUE = the structs are exactly 112 / 252 bytes</xhtml></documentation>
            </variable>
            <variable name="iLastPacketId"><type><INT /></type></variable>
            <variable name="diStep">
              <type><DINT /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">packetId change since the last change seen</xhtml></documentation>
            </variable>
            <variable name="bByteOrderOk">
              <type><BOOL /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">TRUE = packetId grows by 1..255 per change: the service sends little-endian</xhtml></documentation>
            </variable>
            <variable name="nPacketChanges">
              <type><UDINT /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">counts while the PC is alive</xhtml></documentation>
            </variable>
            <variable name="bPacketChanged"><type><BOOL /></type></variable>
            <variable name="tPcSilent"><type><derived name="TON" /></type></variable>
            <variable name="bPcAlive">
              <type><BOOL /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">packetId changed during the last 500 ms</xhtml></documentation>
            </variable>
            <variable name="tTrigger">
              <type><derived name="TON" /></type>
              <documentation><xhtml xmlns="http://www.w3.org/1999/xhtml">test trigger on view 1 every 2 s</xhtml></documentation>
            </variable>
            <variable name="nTrigger"><type><UINT /></type></variable>
            <variable name="sRecipe">
              <type><string length="31" /></type>
              <initialValue><simpleValue value="'SOFTFIELDBUS TEST'" /></initialValue>
            </variable>
            <variable name="i"><type><INT /></type></variable>
          </localVars>
        </interface>
        <body>
          <ST>
            <xhtml xmlns="http://www.w3.org/1999/xhtml">// 0. layout check: must be TRUE, otherwise the structs do not match the 112 / 252 bytes
bLayoutOk := SIZEOF(ST_PI_ToPc) = 112 AND SIZEOF(ST_PI_FromPc) = 252;

// 1. module -&gt; struct: bytes 0..251 of the input module
pByte := ADR(stFromPc);
FOR i := 0 TO 251 DO
    pByte^ := abFromPc[i];             // CODESYS dereferences only a variable, not (p + i)^
    pByte := pByte + 1;
END_FOR

// 2. PC -&gt; PLC: packetId changes every service cycle (Msec, default 10 ms)
bPacketChanged := stFromPc.packetId &lt;&gt; iLastPacketId;
IF bPacketChanged THEN
    // little-endian (right): +1..+255 per change; big-endian (wrong): +256 or more
    diStep := INT_TO_DINT(stFromPc.packetId) - INT_TO_DINT(iLastPacketId);
    IF diStep &lt; 0 THEN
        diStep := diStep + 65536;
    END_IF
    bByteOrderOk := diStep &lt; 256;
    iLastPacketId := stFromPc.packetId;
    nPacketChanges := nPacketChanges + 1;
END_IF
tPcSilent(IN := NOT bPacketChanged, PT := T#500MS);   // restarts on every change
bPcAlive := NOT tPcSilent.Q;

// 3. PLC -&gt; PC: values visible in the service console (key o: recipeId, inputs, outputs)
stToPc.inputs := stToPc.inputs + 1;    // changes every PLC cycle
stToPc.recipeId := 1234;               // the console shows 1234 only if the byte order matches
pByte := ADR(sRecipe);                 // recipe name: STRING(31) = 32 bytes incl. the terminating 0
FOR i := 0 TO 31 DO
    stToPc.recipe[i] := pByte^;
    pByte := pByte + 1;
END_FOR

// 4. test trigger on view 1: new appTag and acquisitionControl = 1 for the first second of every 2 s
tTrigger(IN := NOT tTrigger.Q, PT := T#2S);
IF tTrigger.Q THEN
    nTrigger := nTrigger + 1;
    stToPc.viewControl[0].appTag := UINT_TO_WORD(nTrigger AND 16#0FFF);
END_IF
IF tTrigger.ET &lt; T#1S THEN
    stToPc.viewControl[0].acquisitionControl := 1;
ELSE
    stToPc.viewControl[0].acquisitionControl := 0;
END_IF

// 5. struct -&gt; module: bytes 0..111 of the output module (112..255 are never written: 0)
pByte := ADR(stToPc);
FOR i := 0 TO 111 DO
    abToPc[i] := pByte^;
    pByte := pByte + 1;
END_FOR</xhtml>
          </ST>
        </body>
      </pou>
    </pous>
  </types>
  <instances>
    <configurations />
  </instances>
</project>
```

### Key Concepts
1. **Escape `<`, `>` and `&` in the ST body** (`&lt;`, `&gt;`, `&amp;`). CODESYS refuses the import otherwise.
2. **Keep the two files in sync**: the `.st` file is the readable copy. Change both, or regenerate the XML from the `.st`.
3. **Channel names** in the I/O mapping come from the GSDML module names ("Input 256 bytes (PC to PLC)" ->
   `Input_256_bytes_PC_to_PLC_`). The CODESYS version can change them slightly, so pick the module in the tree.

---

## Verification Checklist

- [ ] `ProInspectPn_PLCopen.xml` is well-formed XML (opens in a browser)
- [ ] CODESYS: Import PLCopenXML creates 5 DUTs and PLC_PRG without errors; Build (F11) with 0 errors
- [ ] Watch: `bLayoutOk` = TRUE right after Login (without the PC)
- [ ] 10j: with the service running and `ByteOrder=little`: `bPcAlive`, `bByteOrderOk` TRUE; console key `o` shows
      recipeId 1234; with `big`, `bByteOrderOk` FALSE and recipeId 3523477504 (0xD2040000)

---

## Next Phase
Continue to [Phase10d-TiaProgramAndGuide.md](Phase10d-TiaProgramAndGuide.md): TIA PLC program and the PLC setup guide.
