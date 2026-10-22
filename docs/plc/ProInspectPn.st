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
    bPcAliveOld    : BOOL;
    nPcLost        : UDINT;            // bPcAlive TRUE -> FALSE: the PC stopped for more than 500 ms (soak tests: must stay 0)

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
IF bPcAliveOld AND NOT bPcAlive THEN
    nPcLost := nPcLost + 1;            // PC silent: service stopped, connection lost or PLC watchdog
END_IF
bPcAliveOld := bPcAlive;

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
