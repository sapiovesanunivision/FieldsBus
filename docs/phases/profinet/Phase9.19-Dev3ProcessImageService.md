# Phase 9.19: Dev.3: PROFINET in ProInspectProcessImageSoftFieldbusService

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Give the ProInspect process-image service a `Transport=profinet` option, so a PROFINET PLC exchanges the same 112 / 252 bytes as with the Hilscher card, EtherNet/IP or Modbus: library sync, DLL project, service settings, `.reg` files, GSDML folder, installer and README. **Status: superseded by [Phase 10](../phase-10-dev3-profinet.md)** (both raw-Ethernet backends, 256 / 256-byte modules, a byte order for every transport); kept for reference. Paths below are relative to the Dev.3 root.

## Documentation References
- [Phase 8 Dev.3 integration](../phase-8-dev3.md)
- [dev3-integration.md](../../dev3-integration.md)
- [Test runbook, section 5](../../profinet-test-runbook.md#5-optional-the-process-image-service)

---

## Step 9.19.1: Before you start

### Purpose
The Dev.3 commit must reference a FieldsBus commit that exists on GitHub.

### Procedure
- FieldsBus: commit the PoC (`feature/profinet-poc`), merge or push it, so `FIELDSBUS_SYNC.txt` points at a
  pushed commit (the sync warns otherwise).
- Dev.3: new branch `dev/<story>/<task>_SoftFieldbusProfinet` from the commit that contains PR 306
  (SoftFieldbus); Dev.3 branches must follow `dev/<story>/<task>` (pre-push hook).
- Every PC that builds Dev.3 with PROFINET (and the build server) needs the **Npcap SDK** and the `NPCAP_SDK`
  environment variable. Without it the DLL still builds, without PROFINET (next steps).
- Design decisions: modules **Output 128 bytes** (PLC -> PC, carries the 112) and **Input 256 bytes**
  (PC -> PLC, carries the 252) from the existing GSDML, the rest is zero padding; byte order **none** by
  default (raw, as the Hilscher service), `swap` available; NIC = `Parameters\profinet\Adapter`, or
  `BindAddress` when empty.

---

## Step 9.19.2: Sync the library into Dev\Sdk\UvX

### Purpose
`tools/sync-to-dev3.ps1` now also mirrors `include/softpn` and `src/pn`.

### Procedure
```powershell
pwsh D:\Univision\Misc\FieldsBus\tools\sync-to-dev3.ps1 -Dev3Root D:\Univision\Dev.3 -WhatIf   # preview
pwsh D:\Univision\Misc\FieldsBus\tools\sync-to-dev3.ps1 -Dev3Root D:\Univision\Dev.3
```
Expected (from e634dd4): **28 added** (`include\softpn\*`, `sources\SoftFieldbus\src\pn\*`) and **5 changed**
(`bytes.hpp`, `export.hpp`, `periodic_timer.hpp`, `fieldbus_device.hpp`, `fieldbus_device.cpp`). The script
warns that source files were added: that is the next step. Commit message: `Sync FieldsBus <hash>`.

---

## Step 9.19.3: DLL project: PROFINET when the Npcap SDK is present

**File:** `Dev/Sdk/UvX/sources/SoftFieldbus/SoftFieldbus1x.vcxproj`

### Purpose
Insert **before** `<Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />`. The block applies to all four configurations and only when `NPCAP_SDK` points to an SDK, so builds without it are unchanged. Add the same files under a "pn" filter in `SoftFieldbus1x.vcxproj.filters`. `src\pn\l2_port_linux.cpp` is mirrored but not compiled.

### Implementation

```xml
<!-- PROFINET (softpn): only with the Npcap SDK (environment variable NPCAP_SDK), as in FieldsBus CMake.
     Without it the DLL builds as before; Transport=profinet then reports "no PROFINET support". -->
<PropertyGroup>
  <SoftFieldbusProfinet Condition="'$(NPCAP_SDK)' != '' and Exists('$(NPCAP_SDK)\Include\pcap.h')">true</SoftFieldbusProfinet>
</PropertyGroup>
<ItemGroup Condition="'$(SoftFieldbusProfinet)' == 'true'">
  <ClCompile Include="src\pn\l2_port_memory.cpp" />
  <ClCompile Include="src\pn\l2_port_npcap.cpp" />
  <ClCompile Include="src\pn\pn_ar.cpp" />
  <ClCompile Include="src\pn\pn_blocks.cpp" />
  <ClCompile Include="src\pn\pn_cyclic.cpp" />
  <ClCompile Include="src\pn\pn_dcp.cpp" />
  <ClCompile Include="src\pn\pn_device.cpp" />
  <ClCompile Include="src\pn\pn_frame.cpp" />
  <ClCompile Include="src\pn\pn_lldp.cpp" />
  <ClCompile Include="src\pn\pn_model.cpp" />
  <ClCompile Include="src\pn\pn_rpc.cpp" />
  <ClCompile Include="src\pn\pn_rta.cpp" />
  <ClCompile Include="src\pn\pn_station_store.cpp" />
  <ClInclude Include="..\..\include\softpn\l2_port.hpp" />
  <ClInclude Include="..\..\include\softpn\pn_device.hpp" />
  <ClInclude Include="src\pn\pn_ar.hpp" />
  <ClInclude Include="src\pn\pn_blocks.hpp" />
  <ClInclude Include="src\pn\pn_cyclic.hpp" />
  <ClInclude Include="src\pn\pn_dcp.hpp" />
  <ClInclude Include="src\pn\pn_defs.hpp" />
  <ClInclude Include="src\pn\pn_frame.hpp" />
  <ClInclude Include="src\pn\pn_lldp.hpp" />
  <ClInclude Include="src\pn\pn_model.hpp" />
  <ClInclude Include="src\pn\pn_rpc.hpp" />
  <ClInclude Include="src\pn\pn_rta.hpp" />
  <ClInclude Include="src\pn\pn_station_store.hpp" />
  <ClInclude Include="src\pn\pn_uuid.hpp" />
</ItemGroup>
<ItemDefinitionGroup Condition="'$(SoftFieldbusProfinet)' == 'true'">
  <ClCompile>
    <AdditionalIncludeDirectories>$(NPCAP_SDK)\Include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    <PreprocessorDefinitions>SOFTFB_HAS_PROFINET=1;%(PreprocessorDefinitions)</PreprocessorDefinitions>
  </ClCompile>
  <Link>
    <AdditionalDependencies>$(NPCAP_SDK)\Lib\x64\wpcap.lib;iphlpapi.lib;delayimp.lib;%(AdditionalDependencies)</AdditionalDependencies>
    <!-- loaded on first use from System32\Npcap: a PC without Npcap gets an error message, not a crash -->
    <DelayLoadDLLs>wpcap.dll;%(DelayLoadDLLs)</DelayLoadDLLs>
  </Link>
</ItemDefinitionGroup>
```

### Key Concepts
1. **Consumers need no define**: `fieldbus_device.hpp` always declares `Transport::ProfinetRt` and `DeviceConfig::profinet`; only the DLL needs `SOFTFB_HAS_PROFINET`.
2. **Update the header comment** of the vcxproj: the mirror now includes `include\softpn` and `src\pn`.

---

## Step 9.19.4: Service settings: Parameters\profinet

**File:** `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService/ProInspectProcessImageSoftFieldbusClient.h`

### Purpose
Add these members at the end of `struct SoftFieldbusServiceConfig` (and `| profinet` to the `transport` comment).

### Implementation

```cpp
std::string profinetAdapter;                                   //!< profinet\Adapter: NIC (friendly name, MAC or IP);
                                                               //!< "" = use BindAddress (the NIC's IP)
std::string profinetStationName = "proinspect";                //!< profinet\StationName: default NameOfStation
std::string profinetPersistPath;                               //!< profinet\PersistPath: NameOfStation set by the PLC;
                                                               //!< "" = %ProgramData%\Univision\<service>\softpn.ini
int profinetVendorId = 65535;                                  //!< profinet\VendorId } must match
int profinetDeviceId = 1;                                      //!< profinet\DeviceId } GSDML\*.xml
int profinetMinUpdateTimeMs = 8;                               //!< profinet\MinUpdateTimeMs: faster Connects are refused
bool profinetSendVlanTag = true;                               //!< profinet\SendVlanTag
bool profinetPromiscuous = false;                              //!< profinet\Promiscuous: 1 if the PLC cannot find the PC
bool profinetByteSwap = false;                                 //!< profinet\ByteOrder: none (default, raw as the
                                                               //!< Hilscher card) | swap (as modbus\ByteOrder)
```

### Key Concepts
1. **Defaults in one place**: the struct defaults are used when a value is missing in the registry.

---

## Step 9.19.5: Read Parameters\profinet in OnInit()

**File:** `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService/ProInspectProcessImageSoftFieldbusService.cpp`

### Purpose
After the `Parameters\modbus` block in `OnInit()`; extend the layout comment above `OnInit()` and the warning text of the `Transport` value with `profinet`. In `overrideTransport()`, route the byte order to PROFINET when that transport is selected.

### Implementation

```cpp
		if (RegOpenKeyExW(hkey, L"profinet", 0, KEY_QUERY_VALUE, &sub) == ERROR_SUCCESS)
		{
			if (readString(sub, L"Adapter", s))
				config_.profinetAdapter = s;
			if (readString(sub, L"StationName", s) && !s.empty())
				config_.profinetStationName = s;
			if (readString(sub, L"PersistPath", s))
				config_.profinetPersistPath = s;
			if (readDword(sub, L"VendorId", v))
				config_.profinetVendorId = v;
			if (readDword(sub, L"DeviceId", v))
				config_.profinetDeviceId = v;
			if (readDword(sub, L"MinUpdateTimeMs", v) && v > 0)
				config_.profinetMinUpdateTimeMs = v;
			if (readDword(sub, L"SendVlanTag", v))
				config_.profinetSendVlanTag = v != 0;
			if (readDword(sub, L"Promiscuous", v))
				config_.profinetPromiscuous = v != 0;
			if (readString(sub, L"ByteOrder", s))
				config_.profinetByteSwap = _stricmp(s.c_str(), "swap") == 0;
			RegCloseKey(sub);
		}

// overrideTransport(): replace the byte-order part with
	bool* swap = config_.transport == softfb::Transport::ProfinetRt ? &config_.profinetByteSwap : &config_.modbusByteSwap;
	if (_stricmp(byteOrder.c_str(), "none") == 0)
		*swap = false;
	else if (_stricmp(byteOrder.c_str(), "swap") == 0)
		*swap = true;
	else if (!byteOrder.empty())
		return false;
```

### Key Concepts
1. **`readString` buffer** is 256 characters: enough for adapter names and paths under ProgramData.

---

## Step 9.19.6: Start the PROFINET device in open()

**File:** `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService/ProInspectProcessImageSoftFieldbusClient.cpp`

### Purpose
In `open()`, replace the two size lines and add the PROFINET settings before `dc.onLog`; add the helper to the anonymous namespace; change the byte-swap rule in `onInputOutputUpdate()`; log the real sizes.

### Implementation

```cpp
// anonymous namespace: default file for the NameOfStation the PLC assigns over DCP
std::string defaultPersistPath()
{
	char base[MAX_PATH] = {};
	if (GetEnvironmentVariableA("ProgramData", base, MAX_PATH) == 0)
		return "softpn.ini";
	std::string dir = std::string(base) + "\\Univision";
	CreateDirectoryA(dir.c_str(), NULL); // fails harmlessly if it exists
	dir += "\\ProInspectProcessImageSoftFieldbusService";
	CreateDirectoryA(dir.c_str(), NULL);
	return dir + "\\softpn.ini";
}

// file scope: the PROFINET modules (GSDML) are 128 / 256 bytes and carry the wire structs plus zero padding
static_assert(sizeof(HilscherProcessImageInputData) <= 128, "PLC -> PC data must fit the 'Output 128 bytes' module");
static_assert(sizeof(HilscherProcessImageOutputData) <= 256, "PC -> PLC data must fit the 'Input 256 bytes' module");

// open(): sizes
	const bool profinet = config_.transport == softfb::Transport::ProfinetRt;
	dc.inputSize = profinet ? 128 : sizeof(HilscherProcessImageInputData);   // PLC -> PC
	dc.outputSize = profinet ? 256 : sizeof(HilscherProcessImageOutputData); // PC -> PLC

// open(): before dc.onLog
	dc.profinet.adapter = !config_.profinetAdapter.empty() ? config_.profinetAdapter : config_.bindAddress;
	dc.profinet.stationName = config_.profinetStationName;
	dc.profinet.persistPath = !config_.profinetPersistPath.empty() ? config_.profinetPersistPath : defaultPersistPath();
	dc.profinet.vendorId = uint16_t(config_.profinetVendorId);
	dc.profinet.deviceId = uint16_t(config_.profinetDeviceId);
	dc.profinet.minUpdateTimeMs = uint32_t(config_.profinetMinUpdateTimeMs);
	dc.profinet.sendVlanTag = config_.profinetSendVlanTag;
	dc.profinet.promiscuous = config_.profinetPromiscuous;
	if (profinet && (dc.profinet.adapter.empty() || dc.profinet.adapter == "0.0.0.0"))
	{
		setError(1, "PROFINET needs a network card: set Parameters\\profinet\\Adapter or BindAddress to the PLC NIC");
		return OperationResult(1, Uvc::String(_T("SoftFieldbus PROFINET: no network card configured")));
	}

// open(): the "started" message prints dc.inputSize / dc.outputSize instead of sizeof(...)

// onInputOutputUpdate(): byte order
	const bool swap = config_.transport == softfb::Transport::ProfinetRt
						  ? config_.profinetByteSwap
						  : (config_.modbusByteSwap && config_.transport != softfb::Transport::EtherNetIP);
```

### Key Concepts
1. **ioRead/ioWrite stay 112 / 252 bytes at offset 0**: the padding bytes are never written, so they stay zero.
2. **Without the fix**, the old rule `transport != EtherNetIP` would apply the Modbus swap to PROFINET.
3. **ProgramData**: the service runs as `.\ProInspect`; `C:\Windows\System32` (its working folder) is not writable.

---

## Step 9.19.7: Command line help

**File:** `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService/ProInspectProcessImageSoftFieldbusServiceMain.cpp`

### Purpose
Add `profinet` to the transport lists (file header, the "unknown transport" message and the usage line). `-process profinet 0 swap` sets the PROFINET byte order (the port is ignored for PROFINET).

### Implementation

```cpp
wprintf(L" -process [transport [port [swap|none]]]	to run the service as process (transport: eip, modbus-tcp, modbus-udp, modbus, profinet).\n");
```

### Key Concepts
1. **No new options**: everything else comes from the registry.

---

## Step 9.19.8: Registry files

### Purpose
One `.reg` per transport, each writing the whole tree.

### Procedure
- New `Registry\ProInspectProcessImageSoftFieldbusService_profinet.reg` (copy of `_eip.reg`, `"Transport"="profinet"`,
  `"BindAddress"` = the PLC NIC's IP).
- Add the `Parameters\profinet` key to **all four** files, so any import switches transport cleanly.
- Save as **UTF-16 LE with BOM** like the existing files (`regedit` shows an error for UTF-8 files with non-ASCII text).

```reg
[HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\ProInspectProcessImageSoftFieldbusService\Parameters\profinet]
; NIC facing the PLC: friendly name ("Ethernet 2"), MAC or IP; empty = BindAddress
"Adapter"=""
; default NameOfStation; the PLC can assign another one over DCP (kept in PersistPath)
"StationName"="proinspect"
; empty = %ProgramData%\Univision\ProInspectProcessImageSoftFieldbusService\softpn.ini
"PersistPath"=""
; identity: must match GSDML\GSDML-V2.43-Univision-SoftPN-*.xml (0xFFFF = placeholder until a PI vendor ID exists)
"VendorId"=dword:0000ffff
"DeviceId"=dword:00000001
; fastest update time accepted, ms
"MinUpdateTimeMs"=dword:00000008
"SendVlanTag"=dword:00000001
; 1 only if the PLC cannot find the PC (some NICs drop the DCP multicast)
"Promiscuous"=dword:00000000
; none = structs as in PC memory (as the Hilscher card); swap = WORD/DWORD/float big-endian
"ByteOrder"="none"
```

---

## Step 9.19.9: GSDML folder

### Purpose
The PROFINET counterpart of the `EDS` folder.

### Procedure
- `GSDML\GSDML-V2.43-Univision-SoftPN-20261013.xml`: copy of FieldsBus `gsdml/` (keep it in sync by hand when the
  GSDML changes).
- `GSDML\README.md`: import the file into the engineering tool; device "SoftPN device"; **slot 1 = "Input 256 bytes
  (PC to PLC)"**, **slot 2 = "Output 128 bytes (PLC to PC)"**; the first 252 / 112 bytes have the same layout
  and offsets as the EtherNet/IP assemblies (README "PLC side"), the rest is zero; NameOfStation =
  `StationName`, IP = the PC's NIC IP; update time 8 ms or more; byte order as `ByteOrder`.

---

## Step 9.19.10: Installer (Inno Setup)

### Purpose
`Dev/Applicazioni/ProInspect/ProVision/Sources/Installatori/ProInspectProcessImageSoftFieldbusService/Source/ProInspectProcessImagesSoftFieldbusService.iss`

### Procedure
```ini
[Tasks]
Name: "transport\profinet"; Description: "PROFINET IO-Device (needs Npcap; modules Input 256 / Output 128 bytes)"; Flags: exclusive unchecked

[Files]
Source: "..\..\UvpProcessImage\ProInspectProcessImageSoftFieldbusService\GSDML\*.xml"; DestDir: "{#UV_SYSTEM_DIR}\GSDML"; Flags: ignoreversion
Source: "..\..\UvpProcessImage\ProInspectProcessImageSoftFieldbusService\GSDML\README.md"; DestDir: "{#UV_SYSTEM_DIR}\GSDML"; Flags: ignoreversion

[Registry]
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}"; ValueType: string; ValueName: "Transport"; ValueData: "profinet"; Flags: uninsdeletekey; Tasks: transport\profinet
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: string; ValueName: "Adapter"; ValueData: ""; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: string; ValueName: "StationName"; ValueData: "proinspect"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: string; ValueName: "PersistPath"; ValueData: ""; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "VendorId"; ValueData: "65535"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "DeviceId"; ValueData: "1"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "MinUpdateTimeMs"; ValueData: "8"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "SendVlanTag"; ValueData: "1"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: dword; ValueName: "Promiscuous"; ValueData: "0"; Flags: uninsdeletekey createvalueifdoesntexist
Root: "HKLM"; Subkey: "{#UV_PARAMETERS}\profinet"; ValueType: string; ValueName: "ByteOrder"; ValueData: "none"; Flags: uninsdeletekey createvalueifdoesntexist
```
- Firewall: no change - the existing rule "ProInspect SoftFieldbus" allows the service executable on every port
  (PROFINET RPC is UDP 34964).
- Npcap is **not** bundled (free licence: no redistribution). Add a `[Code]` check: if `transport\profinet` is
  selected and `{sys}\Npcap\wpcap.dll` does not exist, show "Install Npcap from https://npcap.com before starting
  the service" (`WizardIsTaskSelected('transport\profinet')`, `FileExists(ExpandConstant('{sys}\Npcap\wpcap.dll'))`).
- After installing with PROFINET, set `Parameters\profinet\Adapter` (or `BindAddress`) to the PLC NIC.

---

## Step 9.19.11: Service README

### Purpose
`Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService/README.md`

### Procedure
- Intro and **Settings**: `Transport` gains `profinet`; new table **`Parameters\profinet`** (values of the `.reg`
  above, with Adapter falling back to BindAddress).
- **Running**: `-process profinet [0 [swap|none]]`.
- **PLC side**: new section "PROFINET (CODESYS, TIA Portal, ...)": Npcap prerequisite, GSDML import, module
  choice 256 / 128, NameOfStation and IP, update time >= 8 ms, byte order, firewall note; the offset tables of
  the existing sections apply to the first 252 / 112 bytes.
- **Diagnostics**: `statusDescription` shows e.g. "PROFINET RT: ConnectedRun"; status 1 = waiting for the
  controller or still parameterising.
- **Troubleshooting**: rows from the [test runbook](../../profinet-test-runbook.md#8-troubleshooting) (scan does not
  find the PC, IP refused, ModuleDiff, firewall).

---

## Step 9.19.12: Build and test

### Purpose
Same gates as phase 8, plus a PROFINET connection with the service.

### Procedure
- Build `SoftFieldbus1x` and the service in **Debug_2026, Release_2026, Debug_2022, Release_2022**, with and
  without `NPCAP_SDK` (without: everything builds, `Transport=profinet` logs "no PROFINET support").
  Debug builds run code analysis with `/WX` (the softpn sources are `/analyze` clean in FieldsBus).
- Existing transports unchanged: `-process eip` and `-process modbus-tcp` with `ProInspectProcessImagePlcSim`.
- PROFINET: `ProInspectProcessImageService -process`, then `ProInspectProcessImageSoftFieldbusService -process profinet`
  with `Parameters\profinet\Adapter` set; CODESYS project with "Input 256 bytes" / "Output 128 bytes" ->
  status "PROFINET RT: ConnectedRun", ProInspect sees the PLC data; `swap` and `none` checked against a known value.
- Results in `docs/phases/phase-9-profinet.md` (Dev.3 commit hash), like the phase 8 results tables.

---

## Verification Checklist

- [ ] FieldsBus commit pushed; Dev.3 branch `dev/<story>/<task>` created
- [ ] Sync: 28 added / 5 changed; vcxproj (+ .filters) updated
- [ ] All four configurations build, with and without `NPCAP_SDK`; Debug code analysis clean
- [ ] EtherNet/IP and Modbus still pass with the PLC simulator
- [ ] `Transport=profinet`: CODESYS connects (Input 256 / Output 128), ProInspect data correct, `ByteOrder` none/swap verified
- [ ] `.reg` files (4, UTF-16), GSDML folder, installer task + Npcap check, README updated

---

## Next Phase
Continue to [Phase9.20-NdisBackend.md](Phase9.20-NdisBackend.md): Own NDIS protocol driver backend (no Npcap).
