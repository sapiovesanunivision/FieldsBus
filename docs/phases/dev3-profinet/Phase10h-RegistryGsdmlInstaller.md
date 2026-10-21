# Phase 10h: Registry files, GSDML folder, installer

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repository: **Dev.3**
> `dev/4516/4526-ProcessImage`. Status: planned, not built. `D3` =
> `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService`.

## Goal
Everything around the service for PROFINET and the new byte order: a fourth `.reg` file and the new keys in all
four, the `GSDML` folder next to `EDS`, and the installer (task, files, registry values, ProgramData folder, and a
prerequisite check for Npcap or the driver).

## Documentation References
- [Phase 8](../phase-8-dev3.md) 8e follow-up: registry layout and `.reg` files
- `Dev/Applicazioni/ProInspect/ProVision/Sources/Installatori/ProInspectProcessImageSoftFieldbusService/Source/ProInspectProcessImagesSoftFieldbusService.iss`
- [Inno Setup help](https://jrsoftware.org/ishelp/): `[Dirs]` Permissions, `WizardIsTaskSelected`, `NextButtonClick`

---

## Step 10h.1: Registry files

**Files:** `D3/Registry/ProInspectProcessImageSoftFieldbusService_{eip,modbus_tcp,modbus_udp,profinet}.reg`

### Purpose
Each file writes the whole tree and differs from the others **only in `Transport`**, so importing one switches
transport cleanly. The new file is `_profinet.reg`. All four files get `eip\ByteOrder`, the new
`modbus\ByteOrder` names, and the `profinet` key.

### Implementation

Full content of `ProInspectProcessImageSoftFieldbusService_profinet.reg`. The other three are the same text with
`"Transport"="eip"`, `"modbus-tcp"` or `"modbus-udp"` and the matching second comment line.

```reg
Windows Registry Editor Version 5.00

; ProInspectProcessImageSoftFieldbusService settings: PROFINET IO-Device.
; Import (as administrator), then restart the service (or run it with -process).
; The four files differ only in Transport; Parameters\eip, \modbus and \profinet are always written,
; so switching transport only changes Transport.

[HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\ProInspectProcessImageSoftFieldbusService\Parameters]
; cycle between the process image and the fieldbus data, ms
"Msec"=dword:0000000a
; ProInspect process image slot (0..3)
"Slot"=dword:00000000
; 1 = appTag packed as {partId:12, process:4} (as the Hilscher service)
"CompleteAppTag"=dword:00000000
; 1 = message box at start-up to attach a debugger
"DebugMode"=dword:00000000
; eip | modbus-tcp | modbus-udp | modbus (TCP + UDP) | profinet
"Transport"="profinet"
; NIC to listen on (0.0.0.0 = all). PROFINET needs one NIC: set it here or in profinet\Adapter
"BindAddress"="0.0.0.0"

[HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\ProInspectProcessImageSoftFieldbusService\Parameters\eip]
; O->T assembly, PLC -> PC data (112 bytes)
"InputInstance"=dword:00000096
; T->O assembly, PC -> PLC data (252 bytes)
"OutputInstance"=dword:00000064
"ConfigInstance"=dword:00000097
; fastest RPI accepted, microseconds
"MinRpiUs"=dword:000007d0
"ProductName"="ProInspect SoftFieldbus"
; identity checked by the scanner (electronic key): must match EDS\ProInspectSoftFieldbus.eds
"VendorId"=dword:0000ffff
"ProductCode"=dword:00000001
"MajorRevision"=dword:00000001
"MinorRevision"=dword:00000001
; WORD/DWORD/float on the wire: little = as in PC memory (Rockwell, CODESYS); big = high byte first;
; big-wordswap = each WORD high byte first, DWORD/float low word first
"ByteOrder"="little"

[HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\ProInspectProcessImageSoftFieldbusService\Parameters\modbus]
"Port"=dword:000001f6
; 0 = answer any unit id
"UnitId"=dword:00000000
; ffffffff = off; >= 0 also maps the PC -> PLC data into the holding registers from this register
"OutputsInHoldingAt"=dword:ffffffff
; big = each register high byte first, DWORD/float high word first (old "swap"); big-wordswap = low word first;
; little = structs as they are in memory (old "none")
"ByteOrder"="big"

[HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\ProInspectProcessImageSoftFieldbusService\Parameters\profinet]
; NIC facing the PLC: friendly name ("Ethernet 2"), MAC or IP; empty = BindAddress. Must have a static IP
"Adapter"=""
; default NameOfStation = the device name in the PLC project; the PLC can assign another one (kept in PersistPath)
"StationName"="proinspect"
; empty = %ProgramData%\Univision\ProInspectProcessImageSoftFieldbusService\softpn.ini
"PersistPath"=""
; identity: must match GSDML\GSDML-V2.43-Univision-ProInspect-*.xml (0xFFFF = placeholder until a PI vendor ID exists)
"VendorId"=dword:0000ffff
"DeviceId"=dword:00000002
; fastest update time accepted, ms
"MinUpdateTimeMs"=dword:00000008
"SendVlanTag"=dword:00000001
; 1 only if the PLC cannot find the PC (some NICs drop the DCP multicast)
"Promiscuous"=dword:00000000
; raw Ethernet: auto = softpn_ndis driver if installed and running, else Npcap; ndis; npcap
"L2Backend"="auto"
; big = Siemens S7 (default); little = CODESYS; big-wordswap = DWORD/float low word first
"ByteOrder"="big"
```

Write the four files as **UTF-16 LE with BOM**, like the existing ones (`regedit` misreads UTF-8 files with
non-ASCII text). In PowerShell 7:

```powershell
$dir = 'D:\Univision\Dev.3\Dev\Applicazioni\ProInspect\ProVision\Sources\UvpProcessImage\ProInspectProcessImageSoftFieldbusService\Registry'
$text = Get-Content "$dir\ProInspectProcessImageSoftFieldbusService_profinet.reg" -Raw   # after editing it
foreach ($t in @{ eip = 'eip'; modbus_tcp = 'modbus-tcp'; modbus_udp = 'modbus-udp' }.GetEnumerator()) {
    $line2 = @{ eip = 'EtherNet/IP adapter'; modbus_tcp = 'Modbus TCP server'; modbus_udp = 'Modbus UDP server' }[$t.Key]
    $out = $text.Replace('"Transport"="profinet"', "`"Transport`"=`"$($t.Value)`"").Replace('PROFINET IO-Device.', "$line2.")
    Set-Content "$dir\ProInspectProcessImageSoftFieldbusService_$($t.Key).reg" $out -Encoding Unicode -NoNewline
}
```

### Key Concepts
1. **`BindAddress` stays `0.0.0.0`** in all four files, as the rule "differ only in Transport" requires. With PROFINET
   the service then refuses to start until `Adapter` or `BindAddress` is set. That is intended: a PROFINET device
   must know its NIC.
2. **`-Encoding Unicode`** in PowerShell 7 is UTF-16 LE with BOM.

---

## Step 10h.2: GSDML folder

**Files:** `D3/GSDML/GSDML-V2.43-Univision-ProInspect-20261021.xml`, `D3/GSDML/README.md`

### Purpose
The PROFINET counterpart of the `EDS` folder: the file the PLC engineer imports, and a short README next to it.

### Implementation

Copy the GSDML from FieldsBus `gsdml/` (10b). It is not part of the sync, so copy it again by hand when it changes:

```powershell
Copy-Item D:\Univision\Misc\FieldsBus\gsdml\GSDML-V2.43-Univision-ProInspect-20261021.xml `
          D:\Univision\Dev.3\Dev\Applicazioni\ProInspect\ProVision\Sources\UvpProcessImage\ProInspectProcessImageSoftFieldbusService\GSDML\
```

`D3/GSDML/README.md`:

```markdown
# GSDML-V2.43-Univision-ProInspect-*.xml

Describes ProInspectProcessImageSoftFieldbusService (`Transport = profinet`) as the PROFINET IO-Device
**ProInspect SoftFieldbus** (Conformance Class A, RT_CLASS_1). Import it into the PLC's engineering tool
(TIA Portal, CODESYS, ...). Master copy: FieldsBus `gsdml/`.

**The PC needs raw Ethernet access:** Npcap (https://npcap.com) or the softpn_ndis driver.

## Modules

Both modules are inserted automatically and cannot be changed.

| Slot | Module | Content |
|---|---|---|
| 1 | Input 256 bytes (PC to PLC) | bytes 0..251 = PC → PLC data, 252..255 = 0 |
| 2 | Output 256 bytes (PLC to PC) | bytes 0..111 = PLC → PC data, 112..255 not used |

The byte offsets of the 252 / 112 bytes are those of `..\README.md`, section "PLC side" (the same as EtherNet/IP and
Modbus). WORD / DWORD / REAL follow `Parameters\profinet\ByteOrder`: **`big` for Siemens** (default), **`little`
for CODESYS**.

## Identity and settings

| | GSDML | Service registry `Parameters\profinet` |
|---|---|---|
| Vendor | 0xFFFF "Univision" (placeholder) | `VendorId` = 65535 |
| Device | 0x0002 "ProInspect SoftFieldbus" | `DeviceId` = 2 |
| Device name | `proinspect` (proposed) | `StationName` = proinspect |
| IP address | set in the PLC project | the static IP of the NIC in `Adapter` / `BindAddress` |
| Update time | 8 ms or more | `MinUpdateTimeMs` = 8 |

## PLC programs

Test programs and a step-by-step guide are in FieldsBus `docs/plc/` (`ProInspectPn_PLCopen.xml` for CODESYS,
`ProInspectPn.scl` for TIA Portal) and `docs/SoftFieldbusPnPLCSetup.md`.

## Tested

See FieldsBus `docs/phases/phase-10-dev3-profinet.md`.
```

### Key Concepts
1. **Why the PLC files are not copied here**: they are test programs, not product parts. The EDS folder does not
   carry the EtherNet/IP ones either.

---

## Step 10h.3: Installer

**File:** `Dev/Applicazioni/ProInspect/ProVision/Sources/Installatori/ProInspectProcessImageSoftFieldbusService/Source/ProInspectProcessImagesSoftFieldbusService.iss`

### Purpose
PROFINET as a fourth transport choice, the GSDML files, the new registry values, a writable folder for the
NameOfStation file, and a warning when neither Npcap nor the driver is installed. Npcap is **not** bundled, because
the free licence does not allow redistribution. The driver has its own package (D5).

### Implementation

```ini
; header comment (line 2)
; ProInspectProcessImageSoftFieldbusService: process image over EtherNet/IP, Modbus TCP/UDP or PROFINET (SoftFieldbus).

[Tasks]
; after transport\modbusudp
Name: "transport\profinet"; Description: "PROFINET IO-Device (needs Npcap or the softpn_ndis driver; GSDML in System\GSDML)"; Flags: exclusive unchecked

[Dirs]
; NameOfStation assigned by the PLC (Parameters\profinet\PersistPath default); the service account must write it
Name: "{commonappdata}\Univision\ProInspectProcessImageSoftFieldbusService"; Permissions: users-modify

[Files]
; after the EDS lines
;GSDML: import into the PLC engineering tool (PROFINET IO-Controller)
Source: "..\..\UvpProcessImage\ProInspectProcessImageSoftFieldbusService\GSDML\*.xml"; DestDir: "{#UV_SYSTEM_DIR}\GSDML"; Flags: ignoreversion
Source: "..\..\UvpProcessImage\ProInspectProcessImageSoftFieldbusService\GSDML\README.md"; DestDir: "{#UV_SYSTEM_DIR}\GSDML"; Flags: ignoreversion

[Registry]
; comment above the service parameters: "per transport in Parameters\eip, \modbus and \profinet"
; Transport, after the modbus-udp line
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}"; ValueType: string; ValueName: "Transport"; ValueData: "profinet"; Flags: uninsdeletekey; Tasks: transport\profinet
; eip, after MinorRevision
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\eip"; ValueType: string; ValueName: "ByteOrder"; ValueData: "little"; Flags: uninsdeletekey createvalueifdoesntexist
; modbus: replace the ByteOrder line ("swap" = "big"; an existing value is kept)
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\modbus"; ValueType: string; ValueName: "ByteOrder"; ValueData: "big"; Flags: uninsdeletekey createvalueifdoesntexist
; profinet: identity must match GSDML\GSDML-V2.43-Univision-ProInspect-*.xml
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: string; ValueName: "Adapter"; ValueData: ""; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: string; ValueName: "StationName"; ValueData: "proinspect"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: string; ValueName: "PersistPath"; ValueData: ""; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "VendorId"; ValueData: "65535"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "DeviceId"; ValueData: "2"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "MinUpdateTimeMs"; ValueData: "8"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "SendVlanTag"; ValueData: "1"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "Promiscuous"; ValueData: "0"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: string; ValueName: "L2Backend"; ValueData: "auto"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: string; ValueName: "ByteOrder"; ValueData: "big"; Flags: uninsdeletekey createvalueifdoesntexist

[Run]
; firewall comment: "(EtherNet/IP 44818 + UDP I/O, Modbus 502, or PROFINET RPC UDP 34964)" - the rule itself is unchanged
```

```pascal
[Code]
// after GetRunAsProInspect()

/////////////////////////////////////////////////////////////////////
// PROFINET needs raw Ethernet: Npcap (not redistributable with the free licence) or the softpn_ndis driver
function RawEthernetInstalled(): Boolean;
begin
  Result := FileExists(ExpandConstant('{sys}\Npcap\wpcap.dll'))
            or RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\SoftPnNdis');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectTasks) and WizardIsTaskSelected('transport\profinet') and not RawEthernetInstalled() then
    MsgBox('PROFINET needs raw Ethernet access, and neither Npcap nor the softpn_ndis driver is installed.' + #13#10 +
           'Install Npcap from https://npcap.com (or the softpn_ndis driver package) before starting the service.' + #13#10 + #13#10 +
           'After the installation, set Parameters\profinet\Adapter (or BindAddress) to the network card facing the PLC.',
           mbInformation, MB_OK);
end;
```

### Key Concepts
1. **A warning, not a stop**: Npcap can be installed after the service, and the service starts with a clear error until then.
2. **`{sys}\Npcap`**: Npcap installs `wpcap.dll` in both `System32\Npcap` and `SysWOW64\Npcap`, so the check works
   whether the setup runs in 32- or 64-bit mode.
3. **`SoftPnNdis`** is the driver's service name (`net start SoftPnNdis`). If D5 renames it, change it here.
4. **`users-modify` on the ProgramData folder**: the service may run as `.\ProInspect`, which cannot write a folder an
   administrator created without this permission.
5. **Existing installations** keep `modbus\ByteOrder = "swap"` (`createvalueifdoesntexist`), which 10f reads as `big`.

---

## Verification Checklist

- [ ] Four `.reg` files, UTF-16 LE BOM (`Format-Hex -Count 2` shows `FF FE`), differing only in `Transport` and line 3
- [ ] Importing `_profinet.reg` then `_eip.reg` switches back cleanly (eip run unchanged)
- [ ] `GSDML\` contains the XML and README
- [ ] Installer compiles with Inno Setup 6 (`ISCC ...145_x64.iss`)
- [ ] On a test VM (admin rights, not the dev PC): PROFINET task without Npcap shows the message; registry has
      `Parameters\profinet`; `C:\ProgramData\Univision\ProInspectProcessImageSoftFieldbusService` exists and
      `Users` can modify it; `System145_x64\GSDML` present

---

## Next Phase
Continue to [Phase10i-ReadmeAndPlcSim.md](Phase10i-ReadmeAndPlcSim.md): service README and PLC simulator.
