# Phase 10f: Byte order for all transports

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repository: **Dev.3**
> `dev/4516/4526-ProcessImage`. **Status: implemented 2026-10-21** (Dev.3, uncommitted); code as below. `D3` =
> `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService`.

## Goal
Replace the Modbus-only `swap|none` flag with a byte order for **every** transport: `little`, `big` (ABCD) or
`big-wordswap` (CDAB). The setting is a `ByteOrder` value in the transport's own registry subkey (`eip`, `modbus`,
`profinet`). Existing installations behave as before: EtherNet/IP stays little-endian, Modbus `swap` keeps working
as `big`.

## Documentation References
- [Phase 10 decisions](../phase-10-dev3-profinet.md#decisions)
- `..\ProInspectProcessImageHilscherService\ProInspectProcessImageHilscherData.h`: the wire structs
- Today's code: `ProInspectProcessImageSoftFieldbusClient::swapForModbus()` and its copy in `ProInspectProcessImagePlcSim.cpp`

---

## Step 10f.1: Shared byte-order header

**File:** `D3/ProcessImageByteOrder.h` (new; add it as `ClInclude` to `ProInspectProcessImageSoftFieldbusService1x.vcxproj`)

### Purpose
One field table for the service and the PLC simulator (10i), replacing the two copies of `swapForModbus()`. It
only depends on the Hilscher wire header, so the simulator can include it from the sibling folder.

### Implementation

```cpp
/* UNIVISION @(#)

ProcessImageByteOrder.h: byte order of the WORD / DWORD / float fields of the Hilscher wire structs
(ProInspectProcessImageHilscherData.h) on the fieldbus. Byte arrays and BYTE members never change.
Each conversion is its own inverse: the same call converts PC -> wire and wire -> PC.

Used by ProInspectProcessImageSoftFieldbusService and ProInspectProcessImagePlcSim.
*/

#pragma once

#include "..\ProInspectProcessImageHilscherService\ProInspectProcessImageHilscherData.h"

#include <cstddef>
#include <cstring>
#include <utility>

/*!
 \brief byte order of the multi-byte fields on the wire; the PC is little-endian
*/
enum class ProcessImageByteOrder
{
	Little,      //!< as in PC memory (old "none"): CODESYS on x86 / ARM, Rockwell
	Big,         //!< ABCD (old "swap"): Siemens S7, Modbus with the high word first
	BigWordSwap, //!< CDAB: each WORD big-endian, DWORD / float with the low word first (Modbus "word swap")
};

/*!
 \brief "little" | "none", "big" | "swap", "big-wordswap" (case-insensitive)
 \return false for anything else (out unchanged)
*/
inline bool parseProcessImageByteOrder(const char* text, ProcessImageByteOrder& out)
{
	if (_stricmp(text, "little") == 0 || _stricmp(text, "none") == 0)
		out = ProcessImageByteOrder::Little;
	else if (_stricmp(text, "big") == 0 || _stricmp(text, "swap") == 0)
		out = ProcessImageByteOrder::Big;
	else if (_stricmp(text, "big-wordswap") == 0)
		out = ProcessImageByteOrder::BigWordSwap;
	else
		return false;
	return true;
}

inline const char* processImageByteOrderName(ProcessImageByteOrder o)
{
	switch (o)
	{
	case ProcessImageByteOrder::Big: return "big";
	case ProcessImageByteOrder::BigWordSwap: return "big-wordswap";
	default: return "little";
	}
}

namespace ProcessImageByteOrderDetail
{
inline void swap16(void* base, size_t offset)
{
	BYTE* p = static_cast<BYTE*>(base) + offset;
	std::swap(p[0], p[1]);
}

// DWORD / float: Big = full reverse (ABCD), BigWordSwap = each word reversed, word order kept (CDAB)
inline void swap32(void* base, size_t offset, ProcessImageByteOrder o)
{
	BYTE* p = static_cast<BYTE*>(base) + offset;
	if (o == ProcessImageByteOrder::Big)
	{
		std::swap(p[0], p[3]);
		std::swap(p[1], p[2]);
	}
	else
	{
		std::swap(p[0], p[1]);
		std::swap(p[2], p[3]);
	}
}
} // namespace ProcessImageByteOrderDetail

/*!
 \brief PLC -> PC data (112 bytes): wire <-> PC
*/
inline void applyByteOrder(HilscherProcessImageInputData& d, ProcessImageByteOrder o)
{
	using namespace ProcessImageByteOrderDetail;
	using T = HilscherProcessImageInputData;
	if (o == ProcessImageByteOrder::Little)
		return;
	for (int i = 0; i < HILSCHERPROCESSIMAGENUMVIEWS; i++)
		swap16(&d, offsetof(T, viewControl) + i * sizeof(HilscherProcessImageViewControl)); // appTag; the 2 BYTEs stay
	swap32(&d, offsetof(T, recipeId), o);
	swap32(&d, offsetof(T, inputs), o);
	for (int i = 0; i < PROCESSIMAGEHANDLEDPARTSNUM; i++)
		swap16(&d, offsetof(T, handled) + i * sizeof(WORD));
	for (int i = 0; i < HILSHERPROCESSIMAGEMAXNUMPROCESS; i++)
		swap16(&d, offsetof(T, externalAppTag) + i * sizeof(WORD));
}

/*!
 \brief PC -> PLC data (252 bytes): PC <-> wire
*/
inline void applyByteOrder(HilscherProcessImageOutputData& d, ProcessImageByteOrder o)
{
	using namespace ProcessImageByteOrderDetail;
	using T = HilscherProcessImageOutputData;
	if (o == ProcessImageByteOrder::Little)
		return;
	swap16(&d, offsetof(T, packetId));
	swap16(&d, offsetof(T, systemStatus));
	for (int i = 0; i < HILSCHERPROCESSIMAGENUMVIEWS; i++)
	{
		swap16(&d, offsetof(T, viewStatus) + i * sizeof(HilscherProcessImageViewStatus));
		swap16(&d, offsetof(T, viewResult) + i * sizeof(HilscherProcessImageViewResult));
	}
	swap32(&d, offsetof(T, measurementsStatus), o);
	for (int i = 0; i < HILSCHERPROCESSIMAGENUMRESULT; i++)
		swap32(&d, offsetof(T, measurements) + i * sizeof(float), o);
	swap32(&d, offsetof(T, outputs), o);
}
```

### Key Concepts
1. **The field table is the existing one**, verified with Modbus in Phase 8 (8e). Only the 32-bit swap gains a second variant.
2. **Wire bytes, lowest offset first**:

   | Value | little | big | big-wordswap |
   |---|---|---|---|
   | WORD 0x1234 | `34 12` | `12 34` | `12 34` |
   | DWORD 0x12345678 | `78 56 34 12` | `12 34 56 78` | `56 78 12 34` |
   | float 1.0 (0x3F800000) | `00 00 80 3F` | `3F 80 00 00` | `00 00 3F 80` |
   | recipe "AB" | `41 42` | `41 42` | `41 42` |

3. **Modbus**: each register is sent high byte first. So `big` = DWORD/float as two registers with the high word
   first (as `swap` today), and `big-wordswap` = low word first.

---

## Step 10f.2: Configuration

**File:** `D3/ProInspectProcessImageSoftFieldbusClient.h`

### Purpose
One byte order per transport, with the defaults of the decisions table, plus helpers that return the order of the
active transport.

### Implementation

```cpp
// includes, after <SoftFieldbus\SoftFieldbus.h>
#include "ProcessImageByteOrder.h"

// struct SoftFieldbusServiceConfig: the doc comment above the struct
/*!
 \brief configuration, from HKLM\SYSTEM\CurrentControlSet\Services\ProInspectProcessImageSoftFieldbusService\Parameters
        (common values) and its subkeys Parameters\eip, Parameters\modbus and Parameters\profinet
*/

// struct SoftFieldbusServiceConfig: replace
//     bool modbusByteSwap = true;                                    //!< modbus\ByteOrder: swap (default) | none
// with
	ProcessImageByteOrder eipByteOrder = ProcessImageByteOrder::Little;   //!< eip\ByteOrder
	ProcessImageByteOrder modbusByteOrder = ProcessImageByteOrder::Big;   //!< modbus\ByteOrder (old "swap" = big)
	ProcessImageByteOrder profinetByteOrder = ProcessImageByteOrder::Big; //!< profinet\ByteOrder (Siemens S7)

// struct SoftFieldbusServiceConfig: at the end
	/*!
	 \brief byte order of the active transport
	*/
	ProcessImageByteOrder byteOrder() const
	{
		switch (transport)
		{
		case softfb::Transport::EtherNetIP: return eipByteOrder;
		case softfb::Transport::ProfinetRt: return profinetByteOrder;
		default: return modbusByteOrder;
		}
	}

	/*!
	 \brief the same, writable (command line override)
	*/
	ProcessImageByteOrder& byteOrderOfTransport()
	{
		switch (transport)
		{
		case softfb::Transport::EtherNetIP: return eipByteOrder;
		case softfb::Transport::ProfinetRt: return profinetByteOrder;
		default: return modbusByteOrder;
		}
	}

// class ProInspectProcessImageSoftFieldbusClient: delete the two swapForModbus() declarations and their comment
```

### Key Concepts
1. **The `profinet` member already exists before 10g**: 10g only adds the other PROFINET settings. A `-process
   profinet` before 10g fails in `FieldbusDevice::start()` because there is no adapter.
2. **`ProfinetRt` is in the enum** since the 10e sync, so the switch compiles.

---

## Step 10f.3: Apply it in the cycle

**File:** `D3/ProInspectProcessImageSoftFieldbusClient.cpp`

### Purpose
Use the active transport's order in both directions. The old rule `modbusByteSwap && transport != EtherNetIP` goes.

### Implementation

```cpp
// anonymous namespace: delete swap16() and swap32() (now in ProcessImageByteOrder.h)

// onInputOutputUpdate(): replace the first lines down to the end of the second block
void ProInspectProcessImageSoftFieldbusClient::onInputOutputUpdate()
{
	REQUIRE0(pOutSM_);
	REQUIRE0(pInSM_);
	if (!device_)
		return;

	const ProcessImageByteOrder order = config_.byteOrder();
	{
		copy(out_, *pOutSM_);
		out_.packetId++;
		HilscherProcessImageOutputData wire = out_;
		applyByteOrder(wire, order);
		device_->ioWrite(0, &wire, sizeof wire);
	}
	{
		HilscherProcessImageInputData wire;
		device_->ioRead(0, &wire, sizeof wire);
		applyByteOrder(wire, order);
		in_ = wire;
		copy(*pInSM_, in_);
	}
	// ... statistics unchanged

// delete the section "Modbus byte order": both swapForModbus() definitions

// open(): the "started" event names the byte order
	_stprintf_s(buf, _T("SoftFieldbus %hs started on %hs, slot %d, PLC -> PC %u bytes, PC -> PLC %u bytes, byte order %hs"),
				softfb::FieldbusDevice::transportName(config_.transport), config_.bindAddress.c_str(), config_.slot,
				unsigned(sizeof(HilscherProcessImageInputData)), unsigned(sizeof(HilscherProcessImageOutputData)),
				processImageByteOrderName(config_.byteOrder()));
```

### Key Concepts
1. **`applyByteOrder` returns at once for `Little`**: EtherNet/IP costs nothing extra.
2. **The "started" line changes again in 10g** (module sizes for PROFINET). Keep the byte-order part.

---

## Step 10f.4: Read `ByteOrder` from every subkey

**File:** `D3/ProInspectProcessImageSoftFieldbusService.cpp`

### Purpose
Read the value in `eip` and `modbus` now (`profinet` in 10g). An unknown value keeps the default and writes a
warning, as an unknown `Transport` does.

### Implementation

```cpp
// anonymous namespace, after readString()
// ByteOrder in a transport subkey. false = a value that is not a byte order (order unchanged).
bool readByteOrder(HKEY sub, ProcessImageByteOrder& order)
{
	std::string s;
	if (!readString(sub, L"ByteOrder", s) || s.empty())
		return true;
	return parseProcessImageByteOrder(s.c_str(), order);
}

// layout comment above OnInit(): replace the Parameters\eip and Parameters\modbus lines with
//   Parameters\eip     InputInstance (150), OutputInstance (100), ConfigInstance (151), MinRpiUs (2000), ProductName,
//                      VendorId (65535), ProductCode (1), MajorRevision (1), MinorRevision (1): as EDS\*.eds;
//                      ByteOrder (little)
//   Parameters\modbus  Port (502), UnitId (0 = any), OutputsInHoldingAt (-1 = off), ByteOrder (big)
//   ByteOrder: little | big | big-wordswap (none = little, swap = big)

// OnInit(), eip block, before RegCloseKey(sub)
			if (!readByteOrder(sub, config_.eipByteOrder))
				WriteEventLogEntry(const_cast<PWSTR>(L"Unknown eip\\ByteOrder (little, big, big-wordswap): using little"),
								   EVENTLOG_WARNING_TYPE);

// OnInit(), modbus block: replace
//			if (readString(sub, L"ByteOrder", s))
//				config_.modbusByteSwap = _stricmp(s.c_str(), "none") != 0;
// with
			if (!readByteOrder(sub, config_.modbusByteOrder))
				WriteEventLogEntry(const_cast<PWSTR>(L"Unknown modbus\\ByteOrder (little, big, big-wordswap): using big"),
								   EVENTLOG_WARNING_TYPE);

// overrideTransport(): replace the byte-order part (the six lines from "if (_stricmp(byteOrder.c_str(), "none")")
bool ProInspectProcessImageSoftFieldbusService::overrideTransport(const std::string& transport, int port, const std::string& byteOrder)
{
	if (!softfb::FieldbusDevice::parseTransport(transport, config_.transport))
		return false;
	if (port > 0)
		config_.modbusPort = port;
	// the byte order applies to the transport just selected
	if (!byteOrder.empty() && !parseProcessImageByteOrder(byteOrder.c_str(), config_.byteOrderOfTransport()))
		return false;
	return true;
}

// status(): the console key t
void ProInspectProcessImageSoftFieldbusService::status()
{
	printf("transport %s, state %s, slot %d, byte order %s\n", softfb::FieldbusDevice::transportName(config_.transport),
		   softfb::FieldbusDevice::stateName(client_.state()), config_.slot, processImageByteOrderName(config_.byteOrder()));
}
```

**File:** `D3/ProInspectProcessImageSoftFieldbusService.h`, comment of `overrideTransport()`:

```cpp
	/*!
	 \brief -process [transport [port [byteorder]]]: overrides the registry, for tests and commissioning
	 \param port      Modbus port; 0 = keep the registry value; ignored by the other transports
	 \param byteOrder little | big | big-wordswap (none | swap) for the selected transport; empty = keep the registry value
	 \return false for an unknown transport or byte order
	*/
```

### Key Concepts
1. **Registry upgrade**: an existing `modbus\ByteOrder = "swap"` is read as `big`, so nothing changes for installed PCs.
2. **The `eip` key gets a value for the first time**. Without it, the default `little` is exactly the behaviour of today.

---

## Step 10f.5: Command line

**File:** `D3/ProInspectProcessImageSoftFieldbusServiceMain.cpp`

### Purpose
The new names in the help and the error text. The parsing itself is unchanged, because `overrideTransport()` does the work.

### Implementation

```cpp
// file header comment
  -process [transport [port [byteorder]]]  run as a console process, with a test REPL; transport (eip,
             modbus-tcp, modbus-udp, modbus), Modbus port (0 = registry) and byte order (little, big,
             big-wordswap; none / swap still accepted) override the registry Parameters

// wmain(), -process branch
				// -process <transport> [<modbus port> [little|big|big-wordswap]]: overrides the registry Parameters
				...
					wprintf(L"unknown transport %s (eip, modbus-tcp, modbus-udp, modbus) or byte order (little, big, big-wordswap)\n", argv[2]);

// wmain(), help
		wprintf(L" -process [transport [port [byteorder]]]	to run the service as process (transport: eip, modbus-tcp, modbus-udp, modbus;\n"
				L"          byte order: little, big, big-wordswap).\n");
```

### Key Concepts
1. **`profinet` is added to these texts in 10g**, together with the transport itself.

---

## Verification Checklist

Results 2026-10-21, dev PC, Release_2026, loopback (`ProInspectProcessImageService -process` + service + the
**old** PLC simulator; keys typed into the hidden consoles with `WriteConsoleInput`). Data check: simulator `d 1234`
and `n` x3 -> service key `o`; service packetId -> simulator `p` twice, 1 s apart.

- [x] The service builds in Debug/Release 2026 and 2022: 0 warnings (code analysis in Debug)
- [x] No `swapForModbus` / `modbusByteSwap` left in the service folder

| Service | Simulator | Started line / key `t` | Service sees (recipeId, inputs) | Simulator sees (packetId) |
|---|---|---|---|---|
| `eip` | `eip` | byte order little | 1234, 3 | 799 -> 986 |
| `modbus-tcp 1502 big` | `--byte-order swap` | big | 1234, 3 | 818 -> 1004 |
| `modbus-tcp 1502 little` | `--byte-order none` | little | 1234, 3 | 801 -> 988 |
| `modbus-udp 1502 swap` (old name) | `--byte-order swap` | big | 1234, 3 | 798 -> 988 |
| `modbus-tcp 1502 big` | `--byte-order none` (**mismatch**) | big | **0xD2040000, 0x03000000** | **7939 -> -9981** |
| `modbus-tcp 1502 big-wordswap` | - | big-wordswap | - | - |
| `modbus-tcp 1502 wrong` | - | "unknown transport ... or byte order (little, big, big-wordswap)", exit 1 | | |

- [ ] `big-wordswap` data test: needs the new PLC simulator (10i)
- [ ] Registry `modbus\ByteOrder = "xyz"`: event-log warning (needs admin rights to write HKLM: test VM / installer test)

---

## Next Phase
Continue to [Phase10g-ProfinetTransport.md](Phase10g-ProfinetTransport.md): PROFINET transport in the service.
