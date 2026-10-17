(* ProInspect process image over EtherNet/IP - CODESYS test code (copy / paste version)
   Device: "ProInspect SoftFieldbus" from ProInspectSoftFieldbus.eds, connection "Exclusive Owner".
   PC side: ProInspectProcessImageSoftFieldbusService -process eip.
   Same code as ProInspectEip_PLCopen.xml (Project -> Import PLCopenXML). Procedure: SoftFieldbusEipPLCSetup.md.

   Layout = ProInspectProcessImageHilscherData.h (#pragma pack 4). Every member is naturally aligned, so the
   CODESYS default layout is the same; PLC_PRG checks it at run time (bLayoutOk must be TRUE).
   Byte order: little-endian on both sides (PC and CODESYS Control Win / x86 / ARM): no swapping.

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

(* ===================== DUT ST_PI_ToPc: PLC -> PC, 112 bytes (O->T assembly 150) ===================== *)
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

(* ===================== DUT ST_PI_FromPc: PC -> PLC, 252 bytes (T->O assembly 100) ===================== *)
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
    // I/O: check the first %QB / %IB address in "ProInspect_SoftFieldbus -> EtherNet/IP I/O Mapping"
    stToPc   AT %QB0 : ST_PI_ToPc;     // PLC -> PC, 112 bytes
    stFromPc AT %IB0 : ST_PI_FromPc;   // PC -> PLC, 252 bytes

    bLayoutOk      : BOOL;             // TRUE = the structs are exactly 112 / 252 bytes
    iLastPacketId  : INT;
    nPacketChanges : UDINT;            // counts while the PC is alive
    tPcSilent      : TON;
    bPcAlive       : BOOL;             // packetId changed during the last 500 ms

    tTrigger       : TON;              // test trigger on view 1 every 2 s
    nTrigger       : UINT;
    sRecipe        : STRING(31) := 'SOFTFIELDBUS TEST';
    pRecipe        : POINTER TO BYTE;
    i              : INT;
END_VAR

(* ===================== PLC_PRG: CODE ===================== *)
// 0. layout check: must be TRUE, otherwise the structs do not match the 112 / 252 bytes
bLayoutOk := SIZEOF(ST_PI_ToPc) = 112 AND SIZEOF(ST_PI_FromPc) = 252;

// 1. PC -> PLC: packetId changes every service cycle (Msec, default 10 ms)
IF stFromPc.packetId <> iLastPacketId THEN
    iLastPacketId := stFromPc.packetId;
    nPacketChanges := nPacketChanges + 1;
    tPcSilent(IN := FALSE);            // restart the silence timer
END_IF
tPcSilent(IN := TRUE, PT := T#500MS);
bPcAlive := NOT tPcSilent.Q;

// 2. PLC -> PC: values visible in the service console (key o: recipeId, inputs, outputs)
stToPc.inputs := stToPc.inputs + 1;    // changes every PLC cycle
stToPc.recipeId := 1234;
pRecipe := ADR(sRecipe);               // recipe name: STRING(31) = 32 bytes incl. the terminating 0
FOR i := 0 TO 31 DO
    stToPc.recipe[i] := pRecipe^;      // CODESYS dereferences only a variable, not (pRecipe + i)^
    pRecipe := pRecipe + 1;
END_FOR

// 3. test trigger on view 1: new appTag and acquisitionControl = 1 for the first second of every 2 s
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
