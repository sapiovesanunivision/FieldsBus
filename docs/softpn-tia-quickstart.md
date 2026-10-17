# SoftPN echo test in TIA Portal: step by step

Short version for the S7-1200 / S7-1500 test. Details, background and troubleshooting:
[softpn-siemens-setup.md](softpn-siemens-setup.md).

**Files you need** (from Stefano):

| File | What it is |
|---|---|
| `GSDML-V2.43-Univision-SoftPN-20261013.xml` | device description, **the version from 2026-10-16** (older copies are refused by TIA) |
| `SoftPN_Echo.scl` | PLC test program (one FB) |
| SoftPN test kit | `pn_device_demo.exe`, `pn_probe.exe` + runtime DLLs, for the PC |

Example addresses below: PLC `192.168.0.1`, PC `192.168.0.10`. Use your own, same subnet.

## 1. PC

1. Give the PC's Ethernet port a **static IP** in the PLC's subnet (e.g. `192.168.0.10` / `255.255.255.0`).
2. Install Npcap (<https://npcap.com>, default options) and unpack the kit, e.g. to `C:\SoftPN`.
3. Network card name: `C:\SoftPN> pn_probe --list` (first column, e.g. `Ethernet`).
4. Allow UDP 34964 in the firewall (administrator PowerShell):

   ```powershell
   New-NetFirewallRule -DisplayName "SoftPN (PROFINET RPC)" -Direction Inbound -Action Allow `
     -Program "C:\SoftPN\pn_device_demo.exe" -Protocol UDP -LocalPort 34964
   ```

5. Start the device and leave the window open:

   ```
   C:\SoftPN> pn_device_demo --adapter Ethernet --name softpn-device --in-size 32 --out-size 32
   ```

## 2. TIA Portal: install the GSDML

**Options -> Manage general station description files (GSD)** -> folder with the XML -> tick
`GSDML-V2.43-Univision-SoftPN-20261013.xml` -> **Install**.

Check you have the new file: `findstr MinDeviceInterval GSDML-V2.43-Univision-SoftPN-20261013.xml` must show
`MinDeviceInterval="256"`.

## 3. TIA Portal: add the device and its two modules

1. **Network view** -> Hardware catalog -> **Other field devices -> PROFINET IO -> I/O -> Univision -> SoftPN ->
   SoftPN device** -> drag it into the network view.
2. Click **Not assigned** on it -> choose the PLC's PROFINET interface.
3. Double-click the device -> **Device view**. In the **Device overview**, slots 1 and 2 are empty. Drag from the
   Hardware catalog (task card on the right, **Module**):
   - **Input 32 bytes (PC to PLC)** -> **slot 1**
   - **Output 32 bytes (PLC to PC)** -> **slot 2**

   Nothing to type: TIA fills in the I/Q addresses itself. **Without these two modules the program in step 5 does
   not compile** ("Tag ... not defined").

## 4. TIA Portal: device properties

Select the SoftPN device -> **Properties -> General -> PROFINET interface [X1]**:

| Setting | Value |
|---|---|
| Ethernet addresses -> IP address | **Set IP address in the project** = **the PC's IP** (`192.168.0.10`). SoftPN refuses any other IP. |
| PROFINET device name | untick "Generate ... automatically", enter **`softpn-device`** (same as `--name`) |
| Advanced options -> Real time settings -> IO cycle -> Update time | **Can be set**, **16 ms** (32 ms if the connection drops). Not below 8 ms. |
| ... -> Accepted update cycles without IO data | **10** |

Then **Compile -> Hardware (rebuild all)**.

## 5. TIA Portal: the test program

1. **PLC_1 -> External source files -> Add new external file** -> `SoftPN_Echo.scl`.
2. Right-click the file -> **Generate blocks from source**. This creates **FB "SoftPN_Echo"**.
   (If you used an older source with module names written inside, e.g. `HMSG_Mng.scl`, delete it: those names
   were only examples.)
3. Open **OB1** (SCL, or an SCL network) and call the FB; accept the instance DB TIA proposes (`SoftPN_Echo_DB`):

   ```pascal
   "SoftPN_Echo_DB"(hwIn  := "softpn-device~Input_32_bytes_(PC_to_PLC)_1",
                    hwOut := "softpn-device~Output_32_bytes_(PLC_to_PC)_1");
   ```

   **Do not copy the two names blindly.** Type `"softpn` and pick them from the auto-completion list, or look
   them up in **PLC tags -> Show all tags -> System constants**: the two entries of data type **Hw_SubModule**
   with "Input" and "Output" in the name. Not `...~X1` (that is the PROFINET interface, not a data module).
4. **Compile -> Software**. No errors expected.

## 6. Download and run

1. Download hardware and software to the PLC, CPU to **RUN**.
2. The device name is set by the PLC at startup (it was set to `softpn-device` in step 4). If the PLC does not find
   the device: **Online -> Assign device name**, pick SoftPN (MAC of the PC), name `softpn-device`.

## 7. What you should see

**PC console:**

```
AR: connected to '...' (<PLC MAC>), update time 16 ms, ...
state: Connecting
AR: ApplicationReady sent to 192.168.0.1:34964
state: DataRun
outputs (PLC -> PC, RUN) 00 00 00 00 05 06 07 08 ...      <- every 500 ms, counting up
stats: tx ... rx ..., max TX period ... us, late 0, ARs 1, aborts 0
```

**TIA Portal online:** SoftPN device and both modules green. In `SoftPN_Echo_DB` (monitor):
`bHeartbeatOk = TRUE`, `bEchoOk = TRUE` (briefly FALSE every 500 ms when the pattern changes),
`iRetRead = 0`, `iRetWrite = 0`.

Then: CPU STOP -> RUN, restart `pn_device_demo`, and 30 minutes running (`aborts` must stay 0).

## 8. If something goes wrong

| Symptom | Cause / fix |
|---|---|
| GSD install fails (SchemaValidation, 0x0001..., 0x0002...) | Old GSDML copy. Take the 2026-10-16 file (step 2 check). |
| Compile: `Tag "...Input_32_bytes..." not defined` | Slots 1/2 empty (step 3), or a wrong name in OB1 (step 5.3). |
| Console stays at `WaitingForController` | Firewall (1.4), not the same subnet, wrong device name. Try `pn_device_demo ... --promisc 1`. |
| `DCP: refused IP ...` | The IP in TIA is not the PC's IP (step 4). |
| `AR: Connect refused, PNIOStatus DB 81 02 0B` | Update time below 8 ms (step 4). |
| `expected configuration differs (ModuleDiffBlock)` / module red | Module sizes in TIA differ from `--in-size` / `--out-size` (both 32). |
| `iRetRead` / `iRetWrite` not 0 | `hwIn` / `hwOut` point to the wrong objects (step 5.3). |
| Connection drops, `aborts` counts up | Use 32 ms and/or a higher "Accepted update cycles without IO data". |

**Send back** (also when it works): PLC order number + firmware, TIA version, the complete console output, a
screenshot of the device online + the diagnostics buffer, and if something failed a Wireshark capture on the PC
(filter `pn_dcp || pn_io || pn_rt || lldp || dcerpc`, `.pcapng`).
