# Phase 10g: PROFINET transport in the service

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repository: **Dev.3**
> `dev/4516/4526-ProcessImage`. **Status: implemented 2026-10-21** (Dev.3, uncommitted). One addition to the plan:
> a fifth `-process` argument for the PROFINET NIC (step 10g.4), so the service can be tested without writing HKLM. `D3` =
> `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService`.

## Goal
`Transport=profinet`: the service becomes the PROFINET IO-Device "ProInspect SoftFieldbus" of the GSDML from 10b.
The settings come from `Parameters\profinet`. The 112 / 252 wire bytes sit at offset 0 of two 256-byte modules,
and the byte order from 10f applies.

## Documentation References
- [Phase 10b](Phase10b-ProInspectGsdml.md): identity and modules the service must match
- [Phase 10a](Phase10a-L2BackendOption.md): `ProfinetOptions::l2Backend`
- `include/softfb/fieldbus_device.hpp` (`DeviceConfig::profinet`)

---

## Step 10g.1: Configuration members

**File:** `D3/ProInspectProcessImageSoftFieldbusClient.h`, struct `SoftFieldbusServiceConfig`

### Purpose
One member per `Parameters\profinet` value, with the defaults in one place: the struct defaults apply when the
registry has no value.

### Implementation

```cpp
// transport member: comment
	softfb::Transport transport = softfb::Transport::EtherNetIP; //!< Transport: eip | modbus-tcp | modbus-udp | modbus | profinet

// after profinetByteOrder (10f)
	std::string profinetAdapter;                     //!< profinet\Adapter: NIC (friendly name, MAC or IP); "" = BindAddress
	std::string profinetStationName = "proinspect";  //!< profinet\StationName: default NameOfStation (GSDML DNS_CompatibleName)
	std::string profinetPersistPath;                 //!< profinet\PersistPath: NameOfStation set by the PLC over DCP;
	                                                 //!< "" = %ProgramData%\Univision\ProInspectProcessImageSoftFieldbusService\softpn.ini
	int profinetVendorId = 65535;                    //!< profinet\VendorId } must match
	int profinetDeviceId = 2;                        //!< profinet\DeviceId } GSDML\GSDML-V2.43-Univision-ProInspect-*.xml
	int profinetMinUpdateTimeMs = 8;                 //!< profinet\MinUpdateTimeMs: faster Connects are refused
	bool profinetSendVlanTag = true;                 //!< profinet\SendVlanTag
	bool profinetPromiscuous = false;                //!< profinet\Promiscuous: 1 only if the PLC cannot find the PC
	std::string profinetL2Backend = "auto";          //!< profinet\L2Backend: auto | ndis | npcap
```

Also update the header comment at the top of the file: "... through SoftFieldbus (EtherNet/IP adapter, Modbus
TCP/UDP server or PROFINET IO-Device) ...".

### Key Concepts
1. **DeviceId 2** is the ProInspect GSDML. The generic SoftPN GSDML (DeviceId 1) has other modules, and the PLC would
   report "wrong device" before even trying the modules.

---

## Step 10g.2: Read `Parameters\profinet`

**File:** `D3/ProInspectProcessImageSoftFieldbusService.cpp`, `OnInit()`

### Purpose
The registry counterpart of the members above, after the `modbus` block.

### Implementation

```cpp
// layout comment above OnInit(): add profinet to Transport and a line for the subkey
//   Parameters         DebugMode, Msec, Slot, CompleteAppTag, Transport (eip | modbus-tcp | modbus-udp | modbus |
//                      profinet), BindAddress
//   Parameters\profinet Adapter ("" = BindAddress), StationName (proinspect), PersistPath ("" = ProgramData),
//                      VendorId (65535), DeviceId (2), MinUpdateTimeMs (8), SendVlanTag (1), Promiscuous (0),
//                      L2Backend (auto | ndis | npcap), ByteOrder (big): as GSDML\*.xml

// Transport warning text
			WriteEventLogEntry(const_cast<PWSTR>(L"Unknown Transport (eip, modbus-tcp, modbus-udp, modbus, profinet): using eip"), EVENTLOG_WARNING_TYPE);

// after the modbus block
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
			if (readString(sub, L"L2Backend", s) && !s.empty())
				config_.profinetL2Backend = s; // checked by FieldbusDevice::start()
			if (!readByteOrder(sub, config_.profinetByteOrder))
				WriteEventLogEntry(const_cast<PWSTR>(L"Unknown profinet\\ByteOrder (little, big, big-wordswap): using big"),
								   EVENTLOG_WARNING_TYPE);
			RegCloseKey(sub);
		}
```

### Key Concepts
1. **`readString` reads at most 255 characters**: enough for adapter names and ProgramData paths.
2. **No validation of `L2Backend` here**: `FieldbusDevice::start()` refuses unknown values with a message that the
   service already logs (10a.2).

---

## Step 10g.3: Start the PROFINET device

**File:** `D3/ProInspectProcessImageSoftFieldbusClient.cpp`

### Purpose
Fill `DeviceConfig::profinet`, set the 256-byte module sizes, and refuse to start without a NIC. Without a NIC,
softpn would pick nothing and the PLC would just never find the PC.

### Implementation

```cpp
// file scope, after the two existing static_asserts
// PROFINET: GSDML modules "Input 256 bytes" / "Output 256 bytes"; the wire structs sit at offset 0, the rest
// stays zero (ioWrite never touches it)
static const unsigned kProfinetModuleBytes = 256;
static_assert(sizeof(HilscherProcessImageInputData) <= kProfinetModuleBytes, "PLC -> PC data must fit the 256-byte module");
static_assert(sizeof(HilscherProcessImageOutputData) <= kProfinetModuleBytes, "PC -> PLC data must fit the 256-byte module");

// anonymous namespace, after copyText()
// Default file for the NameOfStation the PLC assigns over DCP. The service runs as .\ProInspect (or LocalSystem);
// its working folder C:\Windows\System32 is not writable. The installer creates the folder with users-modify (10h).
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

// open(): replace the two size lines
	const bool profinet = config_.transport == softfb::Transport::ProfinetRt;
	dc.inputSize = profinet ? kProfinetModuleBytes : sizeof(HilscherProcessImageInputData);   // PLC -> PC
	dc.outputSize = profinet ? kProfinetModuleBytes : sizeof(HilscherProcessImageOutputData); // PC -> PLC

// open(): after dc.modbus.productCode, before dc.onLog
	dc.profinet.adapter = !config_.profinetAdapter.empty() ? config_.profinetAdapter : config_.bindAddress;
	dc.profinet.stationName = config_.profinetStationName;
	dc.profinet.persistPath = !config_.profinetPersistPath.empty() ? config_.profinetPersistPath : defaultPersistPath();
	dc.profinet.vendorId = uint16_t(config_.profinetVendorId);
	dc.profinet.deviceId = uint16_t(config_.profinetDeviceId);
	dc.profinet.minUpdateTimeMs = uint32_t(config_.profinetMinUpdateTimeMs);
	dc.profinet.sendVlanTag = config_.profinetSendVlanTag;
	dc.profinet.promiscuous = config_.profinetPromiscuous;
	dc.profinet.l2Backend = config_.profinetL2Backend;
	if (profinet && (dc.profinet.adapter.empty() || dc.profinet.adapter == "0.0.0.0"))
	{
		setError(1, "PROFINET needs a network card: set Parameters\\profinet\\Adapter or BindAddress to the PLC NIC");
		return OperationResult(1, Uvc::String(_T("SoftFieldbus PROFINET: no network card configured")));
	}

// open(): the "started" event (10f) with the real sizes and, for PROFINET, the NIC
	_stprintf_s(buf, _T("SoftFieldbus %hs started on %hs, slot %d, PLC -> PC %u bytes, PC -> PLC %u bytes, byte order %hs"),
				softfb::FieldbusDevice::transportName(config_.transport),
				profinet ? dc.profinet.adapter.c_str() : config_.bindAddress.c_str(), config_.slot,
				unsigned(dc.inputSize), unsigned(dc.outputSize), processImageByteOrderName(config_.byteOrder()));
```

### Key Concepts
1. **`ioRead` / `ioWrite` stay at 112 / 252 bytes at offset 0** (10f.3): no change in the cycle. Bytes 112..255 from
   the PLC are ignored, and bytes 252..255 to the PLC stay 0 from `FieldbusDevice`'s zeroed shadow buffer.
2. **`BindAddress` as fallback**: on a PC with one NIC for the PLC, setting `BindAddress` alone is enough for every transport.
3. **Which backend was used**: softpn logs "PROFINET device on <NIC> (...)", and `onLog` writes it to the event
   log. A missing backend fails `start()` with the message from 10a.3, which goes to `errorDescription`.
4. **Status**: `setStatus()` already writes "PROFINET RT: <state>" (`FieldbusDevice::transportName`). Status 1 =
   waiting for the controller or still parameterising, 3 = ConnectedRun.

---

## Step 10g.4: Command line and texts

**File:** `D3/ProInspectProcessImageSoftFieldbusServiceMain.cpp`

### Purpose
Make `profinet` visible as a transport. The port argument is ignored for PROFINET; write `0` to give a byte order.

### Implementation

```cpp
// file header comment
  -process [transport [port [byteorder]]]  run as a console process, with a test REPL; transport (eip,
             modbus-tcp, modbus-udp, modbus, profinet), Modbus port (0 = registry; ignored for eip and profinet)
             and byte order (little, big, big-wordswap) override the registry Parameters

// wmain(), error text
					wprintf(L"unknown transport %s (eip, modbus-tcp, modbus-udp, modbus, profinet) or byte order (little, big, big-wordswap)\n", argv[2]);

// wmain(), help
		wprintf(L" -process [transport [port [byteorder]]]	to run the service as process (transport: eip, modbus-tcp, modbus-udp,\n"
				L"          modbus, profinet; byte order: little, big, big-wordswap). Example: -process profinet 0 little\n");
```

**File:** `D3/ProInspectProcessImageSoftFieldbusService.h` and `.cpp`, top comments: "EtherNet/IP adapter, Modbus
TCP/UDP server or PROFINET IO-Device".

**Added during implementation:** a fifth argument, the PROFINET NIC, because without a registry key (no admin
rights on the dev PC) `-process profinet` could only stop with "no network card configured":

```cpp
// ProInspectProcessImageSoftFieldbusService.h
	bool overrideTransport(const std::string& transport, int port, const std::string& byteOrder = std::string(),
						   const std::string& adapter = std::string());

// ProInspectProcessImageSoftFieldbusService.cpp, first lines of overrideTransport()
	if (!adapter.empty())
		config_.profinetAdapter = adapter;

// ProInspectProcessImageSoftFieldbusServiceMain.cpp: argv[5] -> char adapter[256], passed to overrideTransport();
// help: -process [transport [port [byteorder [adapter]]]] ... Example: -process profinet 0 little Ethernet
```

### Key Concepts
1. **`softfb::FieldbusDevice::parseTransport`** already accepts `profinet`, `pn` and `profinet-rt` (Phase 9.16).
2. **The backend** cannot be set on the command line; use `SOFTPN_L2_BACKEND=ndis|npcap` in the console for a test.

---

## Step 10g.5: First run without a PLC

### Procedure
```bat
cd /d D:\Univision\Dev.3\output\v145\Debug_2026\bin
start ProInspectProcessImageService145_x64d.exe -process
ProInspectProcessImageSoftFieldbusService145_x64d.exe -process profinet 0 big
```
- Without `Adapter` / `BindAddress`: "SoftFieldbus PROFINET: no network card configured".
- With `BindAddress` = the dev PC's NIC IP (registry, or a `.reg` from 10h): "PROFINET device on Ethernet (...)",
  key `t` -> "PROFINET RT, state WaitingForMaster, ..., byte order big".
- `pn_probe --list` (FieldsBus) on the same PC confirms the NIC name and IP.
- `L2Backend=ndis` without the driver: "softpn_ndis driver not running ..." in the console and in `errorDescription`.

---

## Verification Checklist

Results 2026-10-21, dev PC (Release_2026, Npcap, NIC "Ethernet" 10.0.0.168):

- [x] The service builds in Debug/Release 2026 and 2022: 0 warnings
- [x] `-process profinet 0 big` without a NIC: "PROFINET needs a network card: set Parameters\profinet\Adapter or
      BindAddress to the PLC NIC", state Stopped, `x` exits
- [x] `-process profinet 0 little Ethernet`: "PROFINET device on Ethernet (9c:2d:cd:4d:7d:be, 10.0.0.168),
      NameOfStation 'proinspect', input 256 B / output 256 B", started line 256 / 256 bytes, byte order little;
      UDP 34964 bound on 10.0.0.168
- [x] `%ProgramData%\Univision\ProInspectProcessImageSoftFieldbusService\` created on the first start
- [x] `SOFTPN_L2_BACKEND=ndis` (no driver on this PC): "start: softpn_ndis driver not running (install it, then:
      net start SoftPnNdis)", state Stopped
- [x] `-process eip`: 112 / 252 bytes, byte order little (unchanged)
- [x] **First end-to-end run with CODESYS** (Control Win on 10.0.0.128, `ProInspectPn_PLCopen.xml`, ProInspect
      GSDML, 32 ms): connect, write 0x8071, ApplicationReady, ConnectedRun; device green in CODESYS. Service side
      (keys `o`, `e`): recipeId 1234, `inputs` counting, recipe "SOFTFIELDBUS TEST" = what PLC_PRG writes, so
      `little` is right for CODESYS. PLC side: data present; `packetId` negative half of the time because the
      16-bit counter wraps every 655 s (not a byte-order symptom; `bByteOrderOk` is the check). One controller
      abort right after the first connect (CODESYS dropping the AR to the stopped `pn_device_demo`), then steady

---

## Next Phase
Continue to [Phase10h-RegistryGsdmlInstaller.md](Phase10h-RegistryGsdmlInstaller.md): registry files, GSDML folder, installer.
