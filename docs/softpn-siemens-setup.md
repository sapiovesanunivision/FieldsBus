# SoftPN with a Siemens PLC (S7-1200 / S7-1500, TIA Portal)

How to connect a Siemens S7-1200 or S7-1500 PROFINET controller to **SoftPN**, the Univision software PROFINET
device (`pn_device_demo.exe` on a Windows PC). The result is a 32-byte echo test: the PLC writes a pattern, the
PC sends it back together with a heartbeat counter.

> **Status:** SoftPN reached data exchange with the CODESYS PROFINET controller on 2026-10-15 (32 ms, 0 aborts).
> **This is the first test with a Siemens CPU.** Things may fail. When they do, please collect what is listed in
> [What to send back](#what-to-send-back). That is the most useful outcome of the test.

## 1. What you need

| Item | Notes |
|---|---|
| Siemens S7-1200 (FW V4.x) or S7-1500 / ET 200SP CPU | Both are configured the same way in TIA Portal. For S7-300/400 (STEP 7 classic) these steps do not apply. |
| TIA Portal | V17 or newer recommended (the GSDML is schema V2.43). Write down the exact version. |
| Windows 10/11 PC with a wired Ethernet port | The "device". A laptop is fine. |
| Npcap | <https://npcap.com>, free edition (allowed for test use). Default install options. |
| SoftPN test kit | Folder with `pn_device_demo.exe`, `pn_probe.exe`, the runtime DLLs and `GSDML-V2.43-Univision-SoftPN-20261013.xml`. You get it from Stefano. |
| Optional: Wireshark | A capture of the startup is very helpful if something goes wrong. |

## 2. Network

- Connect the PC and the PLC to the same switch, or connect them directly with a cable.
- Give the PC's Ethernet port a **static IP address** in the PLC's subnet, for example PLC `192.168.0.1`, PC
  `192.168.0.10` / `255.255.255.0`.
- **SoftPN keeps the IP address of the Windows network card.** If the PLC tries to set a different IP address,
  SoftPN refuses it. The IP address of the SoftPN device in TIA Portal must therefore be **exactly the PC's IP**.

## 3. PC side

1. Install Npcap and unpack the test kit, e.g. to `C:\SoftPN`.
2. Find the name of the network card:

   ```
   C:\SoftPN> pn_probe --list
   ```

   Every line shows the name, MAC address and IP address, for example
   `Ethernet  9c:2d:cd:4d:7d:be  192.168.0.10  Intel(R) Ethernet Connection I219-LM`.
   The name is the first column ("Ethernet" here).
3. Firewall: SoftPN receives the PROFINET connection requests on **UDP port 34964**. Either allow the program when
   Windows asks the first time (tick the network type in use), or add a rule in an **administrator** PowerShell:

   ```powershell
   New-NetFirewallRule -DisplayName "SoftPN (PROFINET RPC)" -Direction Inbound -Action Allow `
     -Program "C:\SoftPN\pn_device_demo.exe" -Protocol UDP -LocalPort 34964
   ```

4. Start the device (in a normal `cmd` window, keep it open):

   ```
   C:\SoftPN> pn_device_demo --adapter Ethernet --name softpn-device --in-size 32 --out-size 32
   ```

   | Option | Meaning |
   |---|---|
   | `--adapter` | network card name from step 2 |
   | `--name` | PROFINET device name. Must be the same as in TIA Portal. |
   | `--in-size` / `--out-size` | 32 bytes each. Must match the modules in TIA Portal. |
   | `--promisc 1` | only if the PLC does not find the device (see Troubleshooting). Kits built before 2026-10-16 always need it. |

   Expected output:

   ```
   PROFINET device on Ethernet (9c:2d:cd:4d:7d:be, 192.168.0.10), NameOfStation 'softpn-device', input 32 B / output 32 B
   state: WaitingForController
   running, Ctrl+C to quit
   ```

   Every 10 s it prints a `stats:` line.

## 4. TIA Portal

### 4.1 Install the GSDML

**Options -> Manage general station description files (GSD)** -> select the kit folder -> tick
`GSDML-V2.43-Univision-SoftPN-20261013.xml` -> **Install**.

If TIA Portal refuses the file (schema version, vendor ID), note the message and your TIA version and stop here.

Known (2026-10-16): TIA Portal checks the GSDML much more strictly than CODESYS. The first versions were refused;
the repo file (same name) now installs. Fixed, in the order TIA reported them:

| TIA check | Fix in the GSDML |
|---|---|
| SchemaValidation: required `NameOfStationNotTransferable` missing (DAP1) | `NameOfStationNotTransferable="false"` |
| 0x00010026_5: `schemaLocation` must name `..\xsd\GSDML-DeviceProfile-vX.Y.xsd` | backslashes in the path |
| 0x00020020_16: no SendClock x ReductionRatio equals `MinDeviceInterval` | `MinDeviceInterval="256"` (8 ms, was 32 = 1 ms) and an **explicit** `ReductionRatio` list |
| 0x00010030_4 (no text) on `TimingProperties` | full list `ReductionRatio="1 2 4 8 16 32 64 128 256 512"` (a list starting at 8 is refused; without the attribute 0x00020020_16 fails) |
| 0x00020020_10 / _11: `PTP_BoundarySupported` / `DCP_BoundarySupported` must be "true" (PNIO_Version >= V2.31) | both "true" on the interface (boundary writes to slot 0 are accepted and ignored) |
| 0x00033002_2: `NumberOfDeviceAccessAR` without `DeviceAccessSupported="true"` | attribute removed |

### 4.2 Add the device

1. **Network view** -> hardware catalog -> **Other field devices -> PROFINET IO -> I/O -> Univision -> SoftPN ->
   SoftPN device** -> drag it into the network view.
2. Click **Not assigned** on the device and choose the PLC's PROFINET interface. A PROFINET IO system line appears.
3. Open the **Device view** of the SoftPN device and drag from the catalog into the device overview:
   - slot 1: **Input 32 bytes (PC to PLC)**
   - slot 2: **Output 32 bytes (PLC to PC)**

   Only these two slots exist. The sizes must match `--in-size` / `--out-size`.

### 4.3 Device properties

Select the SoftPN device -> **Properties -> General -> PROFINET interface [X1]**:

| Setting | Value |
|---|---|
| Ethernet addresses -> IP address | **Set IP address in the project** = the PC's IP (e.g. `192.168.0.10`), mask as the PC |
| PROFINET device name | untick "Generate PROFINET device name automatically", enter **`softpn-device`** (same as `--name`) |
| Advanced options -> Real time settings -> IO cycle -> **Update time** | **Can be set**, **16 ms** (32 ms if you see aborts). SoftPN refuses anything below 8 ms. Windows is not a real-time system, so do not go below 16 ms on the first test. |
| Advanced options -> Real time settings -> IO cycle -> **Accepted update cycles without IO data** | **10** |
| Port interconnection / topology | leave empty (not configured) |
| MRP, Shared device, IRT | off / not used |

The PLC's own PROFINET interface keeps its defaults (send clock 1 ms).

### 4.4 Device name

The name is already set by `--name` on the PC, so **Assign device name** is not necessary. If you use it anyway
(network view -> right-click the device -> Assign device name), SoftPN accepts it and stores it, and the console
shows `DCP: NameOfStation = '...'`. Names: lower case, digits, `-` and `.` only.

To check that the PLC network sees the PC: **Online access -> your network card -> Update accessible devices**.
SoftPN appears as "Univision SoftPN" with the device name. **Flash LED** prints
`*** DCP signal: this is the device you are looking for ***` in the console.

### 4.5 Test program

Data layout seen from the PLC:

| Bytes | Input module (PC -> PLC) | Output module (PLC -> PC) |
|---|---|---|
| 0..3 | heartbeat counter from the PC, little-endian, counts up continuously | not used |
| 4..31 | copy of output bytes 4..31 (echo) | test pattern written by the PLC |

Ready-made source: [plc/SoftPN_Echo.scl](plc/SoftPN_Echo.scl) (**External source files -> Add new external
file -> Generate blocks from source**). There the two `LADDR` values are FB inputs `hwIn` / `hwOut`: call it in
OB1 as `"SoftPN_Echo_DB"(hwIn := <input module>, hwOut := <output module>);`. Or create the FB by hand
(same logic, `LADDR` written inside), call it from **OB1** with an instance DB:

```pascal
FUNCTION_BLOCK "SoftPN_Echo"
{ S7_Optimized_Access := 'TRUE' }
VAR
   abIn          : Array[0..31] of Byte;
   abOut         : Array[0..31] of Byte;
   tPattern      : TON_TIME;
   tSecond       : TON_TIME;
   usPattern     : USInt;
   dwHeartbeat   : DWord;
   dwLastBeat    : DWord;
   bHeartbeatOk  : Bool;   // TRUE: the PC is alive
   bEchoOk       : Bool;   // TRUE: the PC returned our pattern
   iRetRead      : Int;    // 0 = OK
   iRetWrite     : Int;    // 0 = OK
   i             : Int;
END_VAR

BEGIN
    // consistent read of the whole input module (32 bytes)
    #iRetRead := DPRD_DAT(LADDR := "SoftPN_device~Input_32_bytes_(PC_to_PLC)_1", RECORD => #abIn);

    // heartbeat from the PC (bytes 0..3, little-endian); must change at least once per second
    #dwHeartbeat := SHL(IN := BYTE_TO_DWORD(#abIn[3]), N := 24) OR SHL(IN := BYTE_TO_DWORD(#abIn[2]), N := 16)
                 OR SHL(IN := BYTE_TO_DWORD(#abIn[1]), N := 8) OR BYTE_TO_DWORD(#abIn[0]);
    #tSecond(IN := NOT #tSecond.Q, PT := T#1S);
    IF #tSecond.Q THEN
        #bHeartbeatOk := #dwHeartbeat <> #dwLastBeat;
        #dwLastBeat := #dwHeartbeat;
    END_IF;

    // echo check: bytes 4..31 must come back (FALSE for a few cycles right after the pattern changes)
    #bEchoOk := TRUE;
    FOR #i := 4 TO 31 DO
        IF #abIn[#i] <> #abOut[#i] THEN
            #bEchoOk := FALSE;
        END_IF;
    END_FOR;

    // new pattern every 500 ms in bytes 4..31
    #tPattern(IN := NOT #tPattern.Q, PT := T#500MS);
    IF #tPattern.Q THEN
        #usPattern := #usPattern + 1;
    END_IF;
    FOR #i := 4 TO 31 DO
        #abOut[#i] := USINT_TO_BYTE(#usPattern + INT_TO_USINT(#i));
    END_FOR;

    // consistent write of the whole output module
    #iRetWrite := DPWR_DAT(LADDR := "SoftPN_device~Output_32_bytes_(PLC_to_PC)_1", RECORD := #abOut);
END_FUNCTION_BLOCK
```

**`LADDR`** is the hardware identifier of the module. The names above are examples: TIA Portal builds them from
your device and module names. Type `"SoftPN` and pick the two modules from the auto-completion list, or look them
up in **PLC tags -> Show all tags -> System constants** (the two entries of data type `Hw_SubModule` with your
module names).

Compile, download hardware and software, set the CPU to RUN.

## 5. What you should see

**PC console** (a few seconds after the download):

```
AR: connected to 'plc_1...' (<PLC MAC>), update time 16 ms, C_SDU in/out 40/40 bytes, Connect from <PLC IP>:..., output FrameID ...
AR: WaitPrmEnd
state: Connecting
AR: WaitAppReadyCnf
AR: ApplicationReady sent to <PLC IP>:34964
AR: Data
state: DataRun
outputs (PLC -> PC, RUN) 00 00 00 00 05 06 07 08 ...       <- one line every 500 ms, the pattern counting up
stats: tx 625 rx 624, max TX period 16xxx us, late 0, ARs 1, aborts 0
```

**TIA Portal online:** SoftPN device and both modules green, no entries for it in the diagnostics buffer.
In the instance DB: `bHeartbeatOk = TRUE`, `bEchoOk` TRUE most of the time, `iRetRead = iRetWrite = 0`.

Then please also try:
1. CPU **STOP**, wait 5 s, **RUN**: the console shows `outputs (... STOP)` and then RUN again, or a new connection.
2. Close `pn_device_demo` (Ctrl+C), wait 10 s, start it again: the PLC must reconnect by itself.
3. Leave it running for 30 minutes: `aborts` should stay 0.

## 6. Troubleshooting

| Symptom | What to do |
|---|---|
| Console shows nothing after `WaitingForController` | Restart with `--promisc 1` (the network card may drop the PLC's search multicast). Firewall? PC and PLC on the same subnet? Check with "Update accessible devices" (4.4). |
| `DCP: refused IP ...` | The IP in TIA Portal is not the PC's IP. Fix it in 4.3 and download again. |
| `DCP: refused NameOfStation ...` | Invalid name, see 4.4. |
| `AR: Connect refused, PNIOStatus DB 81 02 0B` | Update time below 8 ms. Set 16 ms (4.3). |
| `AR: Connect refused, PNIOStatus ...` (other values) | Copy the full line (it lists the IOCR FrameIDs) and send it. |
| `AR: expected configuration differs (ModuleDiffBlock)` | Module sizes in TIA Portal differ from `--in-size` / `--out-size`. |
| `ApplicationReady sent ... (retry)` repeats | The PLC does not answer on UDP 34964. Send the log and, if possible, a Wireshark capture. |
| Connection drops: `data-hold timer expired`, `aborts` increases | Windows timing. Use 32 ms and/or a higher "Accepted update cycles without IO data". |
| Startup stops after a "Read" | SoftPN only answers the I&M0 record. Send the log / capture: we add what the Siemens CPU asks for. |
| PLC shows the device, but the input data looks shifted | Use `DPRD_DAT` with the module's hardware identifier as above, not a hand-typed `%IB` address. |

## What to send back

- PLC type with order number and firmware version, TIA Portal version.
- The complete console output of `pn_device_demo` (select all, copy into a text file).
- A screenshot of the device view online and the CPU diagnostics buffer entries for the device.
- If something failed: a **Wireshark capture** on the PC's network card from before the download until the error,
  display filter `pn_dcp || pn_io || pn_rt || lldp || dcerpc`, saved as `.pcapng`.
