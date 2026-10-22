# Phase 10i: Service README and PLC simulator

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repository: **Dev.3**
> `dev/4516/4526-ProcessImage`. **Status: implemented 2026-10-22.** README committed in Dev.3 `17a9281f4a` (with
> the PROFINET test findings: fifth `-process` argument, admin console for the driver, 16-bit `packetId` wrap);
> PLC simulator uncommitted. `D3` =
> `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService`.

## Goal
Document PROFINET and the per-transport byte order in the service README. Move the PLC simulator to the shared
byte-order header, so it can test every byte order on EtherNet/IP and Modbus. PROFINET is not simulated, because it
needs a second NIC. It is tested against real controllers in 10j.

## Documentation References
- `D3/README.md` (installed next to the service)
- `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImagePlcSim/ProInspectProcessImagePlcSim.cpp`
- [Phase 10f](Phase10f-ByteOrder.md): `ProcessImageByteOrder.h`

---

## Step 10i.1: Service README

**File:** `D3/README.md`

### Purpose
The README is installed with the service and is what the commissioning engineer reads. Each block below replaces or
extends one section. Keep the rest unchanged.

### Implementation

**Intro** (first paragraph and the diagram):

```markdown
A Windows service that exchanges the ProInspect **process image** with a PLC over **EtherNet/IP**, **Modbus TCP/UDP**
or **PROFINET**, using only the PC's network card. It replaces `ProInspectProcessImageHilscherService`, which needs a
Hilscher cifX card.
...
- **The PC is the device:** the PLC is the EtherNet/IP scanner, the Modbus client or the PROFINET IO-Controller, and
  it connects to the PC.

PLC  <── EtherNet/IP, Modbus or PROFINET ──>  ProInspectProcessImageSoftFieldbusService  <── shared memory ──>  ProInspect
```

**Files**, new rows:

```markdown
| `Registry\ProInspectProcessImageSoftFieldbusService_profinet.reg` | Settings for PROFINET |
| `GSDML\GSDML-V2.43-Univision-ProInspect-*.xml` | PROFINET device description for the PLC's engineering tool (TIA Portal, CODESYS, ...) |
```

**Install**, step 1 and a new note:

```markdown
1. Choose the fieldbus: EtherNet/IP, Modbus TCP, Modbus UDP or PROFINET.
...
**PROFINET** also needs raw Ethernet access: **Npcap** (https://npcap.com, not included in the setup) or the
**softpn_ndis** driver. The setup warns when neither is installed. After the setup, set
`Parameters\profinet\Adapter` (or `BindAddress`) to the network card facing the PLC.
```

**Settings**: `Transport` row, a `ByteOrder` row in the `eip` and `modbus` tables, a byte-order paragraph, and the
new table:

```markdown
| `Transport` | string | `eip` | `eip`, `modbus-tcp`, `modbus-udp`, `modbus` (TCP and UDP together) or `profinet` |
| `BindAddress` | string | `0.0.0.0` | Network card to listen on (`0.0.0.0` = all). PROFINET: the NIC facing the PLC, if `profinet\Adapter` is empty |

(eip table)
| `ByteOrder` | string | `little` | Byte order of WORD, DWORD and float values, see below |

(modbus table)
| `ByteOrder` | string | `big` | Byte order of WORD, DWORD and float values, see below. `swap` (old) = `big` |

**Byte order** (`ByteOrder` in `eip`, `modbus` and `profinet`; byte arrays such as the recipe never change):

| Value | WORD 0x1234 | DWORD 0x12345678 / float | Typical PLC |
|---|---|---|---|
| `little` (old `none`) | `34 12` | `78 56 34 12` | Rockwell, CODESYS (x86, ARM) |
| `big` (old `swap`) | `12 34` | `12 34 56 78` | Siemens S7; Modbus with the high word first |
| `big-wordswap` | `12 34` | `56 78 12 34` | Modbus with the low word first |

**`Parameters\profinet`** (PROFINET)

| Value | Type | Default | Meaning |
|---|---|---|---|
| `Adapter` | string | empty | NIC facing the PLC: name ("Ethernet 2"), MAC or IP. Empty = `BindAddress`. The NIC needs a static IP |
| `StationName` | string | `proinspect` | Default PROFINET device name (NameOfStation). The PLC can assign another one; it is kept in `PersistPath` |
| `PersistPath` | string | empty | File for the assigned name. Empty = `%ProgramData%\Univision\ProInspectProcessImageSoftFieldbusService\softpn.ini` |
| `VendorId` / `DeviceId` | dword | 65535 / 2 | Identity. Must match `GSDML\GSDML-V2.43-Univision-ProInspect-*.xml` |
| `MinUpdateTimeMs` | dword | 8 | Fastest update time accepted; a faster PLC configuration is refused |
| `SendVlanTag` | dword | 1 | 802.1Q priority tag on the cyclic frames |
| `Promiscuous` | dword | 0 | 1 only if the PLC cannot find the PC (some NICs drop the DCP multicast) |
| `L2Backend` | string | `auto` | Raw Ethernet: `auto` (softpn_ndis driver if running, else Npcap), `ndis` or `npcap` |
| `ByteOrder` | string | `big` | See above: `big` for Siemens, `little` for CODESYS |
```

**Running**, the `-process` row:

```markdown
| `...exe -process <transport> [<port> [little\|big\|big-wordswap]]` | The same, but the transport, Modbus port and byte order override the registry (the port is ignored for `eip` and `profinet`: write `0`). No administrator rights are needed, except for PROFINET with the softpn_ndis driver. Example: `-process profinet 0 little` |
```

**PLC side**, after the Modbus section (and the Modbus byte-order paragraph now points to the table above):

```markdown
### PROFINET (TIA Portal, CODESYS, ...)

`GSDML\GSDML-V2.43-Univision-ProInspect-*.xml` describes the service as the IO-Device **ProInspect SoftFieldbus**.
Both modules are inserted when the device is added:

| Slot | Module | Content |
|---|---|---|
| 1 | Input 256 bytes (PC to PLC) | the 252 bytes PC → PLC at offset 0, then 4 bytes 0 |
| 2 | Output 256 bytes (PLC to PC) | the 112 bytes PLC → PC at offset 0, the rest is not used |

1. Install the GSDML in the engineering tool and add the device to the PLC's PROFINET IO system.
2. Device name = `StationName` (`proinspect`); IP address = the static IP of the PC's NIC, set in the project.
3. Update time 8 ms or more (16 ms for a first test); accepted update cycles without IO data: 10.
4. `profinet\ByteOrder`: `big` for Siemens (default), `little` for CODESYS.

Test programs and a step-by-step guide: FieldsBus `docs/plc/ProInspectPn_PLCopen.xml` (CODESYS),
`docs/plc/ProInspectPn.scl` (TIA Portal) and `docs/SoftFieldbusPnPLCSetup.md`.
```

**Testing without a PLC**, options table row and a note:

```markdown
| `--byte-order little\|big\|big-wordswap` | Byte order (default: the service's `ByteOrder` of the transport) |

PROFINET is not simulated: it needs a second network card. Test it with the PLC, or with FieldsBus
`pn_controller_sim` on a second PC.
```

**Diagnostics**, `statusDescription` example and new troubleshooting rows:

```markdown
  - `statusDescription`, for example `EtherNet/IP: ConnectedRun` or `PROFINET RT: ConnectedRun`;

| "no network card configured" (PROFINET) | Set `profinet\Adapter` or `BindAddress` to the NIC facing the PLC. |
| "Npcap is not installed ... or install and start the softpn_ndis driver" | Install Npcap or the driver. |
| The PLC does not find the device (PROFINET) | Device name = `StationName`; NIC in the PLC's subnet; try `Promiscuous` = 1. |
| PLC reports a wrong IP / Set IP refused | The IP in the PLC project must be the PC's static IP: Windows owns the address. |
| Values look wrong | `ByteOrder` of the transport doesn't match the PLC program (see the byte-order table). |
```

The last row replaces "Values look wrong on Modbus".

**Source**: add `ProcessImageByteOrder.h` (byte order, shared with the PLC simulator) to the Service row.

### Key Concepts
1. **One byte-order table for all transports** replaces the Modbus-only paragraph. Modbus keeps its register
   explanation ("each WORD is one register, ...").

---

## Step 10i.2: PLC simulator, byte order for every transport

**File:** `.../ProInspectProcessImagePlcSim/ProInspectProcessImagePlcSim.cpp`

### Purpose
Use `ProcessImageByteOrder.h` instead of the local copy of the field tables, and apply the byte order to EtherNet/IP
too. The 10j regression then covers 3 transports (eip, modbus-tcp, modbus-udp) x 3 byte orders.

### Implementation

```cpp
// header comment, lines 8-9 and the usage
  Modbus       client:  holding registers 0..55 = PLC -> PC (FC16), input registers 0..125 = PC -> PLC (FC04)
  Byte order of WORD / DWORD / float: the service's ByteOrder of the transport (eip: little, modbus: big), or
  --byte-order. PROFINET is not simulated (it needs a second network card).
...
                               [--byte-order little|big|big-wordswap] [--machine MS] [--view N]

// includes, after ProInspectProcessImageHilscherData.h
#include "..\ProInspectProcessImageSoftFieldbusService\ProcessImageByteOrder.h"

// struct Options: replace "bool byteSwap = true; ..." with
	ProcessImageByteOrder eipByteOrder = ProcessImageByteOrder::Little;   // service defaults
	ProcessImageByteOrder modbusByteOrder = ProcessImageByteOrder::Big;
	bool byteOrderSet = false;                                            // --byte-order given
	ProcessImageByteOrder byteOrder = ProcessImageByteOrder::Little;

	bool modbus() const { return transport != "eip" && transport != "ethernetip"; }
	ProcessImageByteOrder effectiveByteOrder() const
	{
		if (byteOrderSet)
			return byteOrder;
		return modbus() ? modbusByteOrder : eipByteOrder;
	}

// loadServiceRegistry(): eip block, after ConfigInstance
		if (regString(sub, L"ByteOrder", s))
			(void)parseProcessImageByteOrder(s.c_str(), o.eipByteOrder); // invalid: keep the service default, as the service does
// loadServiceRegistry(): modbus block, replace the ByteOrder lines
		if (regString(sub, L"ByteOrder", s))
			(void)parseProcessImageByteOrder(s.c_str(), o.modbusByteOrder);

// delete the section "Modbus byte order "swap"": swap16, swap32 and both swapForModbus

// class PlcSim
	explicit PlcSim(const Options& o) : o_(o), order_(o.effectiveByteOrder()) {}

	// start(): the error for an unknown transport
		if (!eip && !tcp && !udp)
		{
			error = o_.transport == "profinet" || o_.transport == "pn"
						? "PROFINET is not simulated (it needs a second network card): test with the PLC or FieldsBus "
						  "pn_controller_sim, or force eip / modbus"
						: "transport must be eip, modbus, modbus-tcp or modbus-udp";
			return false;
		}

	// PC -> PLC: last received data
	HilscherProcessImageOutputData out() const
	{
		HilscherProcessImageOutputData o = {};
		if (eip_)
			eip_->ioRead(0, &o, sizeof o);
		else if (mb_)
			mb_->ioRead(0, &o, sizeof o);
		applyByteOrder(o, order_);
		return o;
	}

private:
	void pushLocked()
	{
		HilscherProcessImageInputData wire = in_;
		applyByteOrder(wire, order_);
		if (eip_)
			eip_->ioWrite(0, &wire, sizeof wire);
		else if (mb_)
			mb_->ioWrite(0, &wire, sizeof wire);
	}

	Options o_;
	ProcessImageByteOrder order_;
	// ... other members unchanged

// parse(): the option
		else if (k == "--byte-order")
		{
			if (!parseProcessImageByteOrder(v, o.byteOrder))
			{
				std::fprintf(stderr, "unknown byte order %s (little, big, big-wordswap)\n", v);
				return false;
			}
			o.byteOrderSet = true;
		}

// main(): usage text
					"                             [--byte-order little|big|big-wordswap] [--machine MS] [--view 1..8]\n"
					"Simulates the PLC of ProInspectProcessImageSoftFieldbusService (EtherNet/IP or Modbus; not PROFINET).\n"
					"Defaults: the service's registry on this PC (Transport, eip and modbus settings incl. ByteOrder) and host\n"
					"127.0.0.1, so no options are needed for a same-PC test.\n");

// main(): the start-up lines
	if (o.modbus())
		std::printf("Modbus port %d unit %d\n", o.port, o.unit);
	else
		std::printf("EtherNet/IP assemblies O->T %d, T->O %d, config %d, T->O on UDP %d\n", o.o2t, o.t2o, o.cfg, o.localPort);
	std::printf("byte order %s (must match the service's %s\\ByteOrder)\n", processImageByteOrderName(o.effectiveByteOrder()),
				o.modbus() ? "modbus" : "eip");
```

### Key Concepts
1. **`order_` is fixed at construction**: the transport cannot change while the simulator runs, so the order cannot either.
2. **`modbus()` also excludes "ethernetip"**: `start()` accepts that spelling, but the old `modbus()` treated it as Modbus.
3. **Include path**: `ProcessImageByteOrder.h` includes the Hilscher header relative to its own folder. MSVC
   searches the including file's directory first, so the simulator's project needs no new include directory.
4. **Repeat the registry rule of the service**: an invalid value keeps the default, so service and simulator agree.

---

## Verification Checklist

Results 2026-10-22, dev PC, Release_2026:

- [x] The PLC simulator builds in Debug/Release 2026 and 2022: 0 warnings; no `swapForModbus` / `byteSwap` left
- [x] `--byte-order xyz`: "unknown byte order xyz (little, big, big-wordswap)" + usage with the new values, exit 1
- [x] `ProInspectProcessImagePlcSim profinet`: "cannot start: PROFINET is not simulated ...", exit 1
- [x] Matrix service x simulator (simulator `d 1234`, `n` x3 -> service key `o`; service packetId -> simulator `p` x2):

| Transport | Service | Simulator | Service sees recipeId / inputs | Simulator packetId |
|---|---|---|---|---|
| eip | little / big / big-wordswap | same | 0x000004D2 / 0x00000003 (all three) | 918 -> 1127, 1044 -> 1274, 943 -> 1149 |
| modbus-tcp | little / big / big-wordswap | same | 0x000004D2 / 0x00000003 (all three) | 866 -> 1122, 1121 -> 1429, 898 -> 1104 |
| modbus-udp | little / big / big-wordswap | same | 0x000004D2 / 0x00000003 (all three) | 1140 -> 1354, 941 -> 1188, 942 -> 1172 |
| eip | little | **big** | 0xD2040000 / 0x03000000 (bytes reversed) | 9476 -> -23547 |
| modbus-tcp | big | **big-wordswap** | 0x04D20000 / 0x00030000 (words swapped; WORDs agree) | 914 -> 1096 |
| modbus-udp | big-wordswap | **little** | 0x0000D204 / 0x00000300 (bytes swapped in each word) | 12035 -> -5629 |

- [ ] Start-up line with the byte order taken from the registry (no service registry key on the dev PC: test PC / VM)

---

## Next Phase
Continue to [Phase10j-BuildAndTest.md](Phase10j-BuildAndTest.md): build and test.
