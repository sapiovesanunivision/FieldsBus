# Spec: Phase 9 – in-house PROFINET RT device (`softpn`) for Windows, on Npcap

> Input for `/spec-to-code`. Tech stack: **C++17, CMake** (see repo `CMakeLists.txt`), Windows 11 + Linux (CI).
> Output: phase documents in `docs/phases/profinet/`, named `Phase9.NN-ShortTitle.md`, each ~30–60 min of work,
> each leaving the build green on Windows (VS 2026, `/W4`, `vs2026-analyze`) and Linux (g++ `-Wall -Wextra -Wpedantic`).

---

## 1. Context

- FieldsBus already provides software-only fieldbus endpoints behind the Hilscher-style process-image API
  `softfb::FieldbusDevice`: an EtherNet/IP adapter (`softeip`) and Modbus TCP/UDP server and client (`softmb`). See `README.md`.
- Management asked for a **PROFINET RT IO-Device** (the PLC is the IO-Controller). This is README roadmap item 9.
- **Decisions:**
  - Our own stack in modern C++, in the style of `softeip`. Not a p-net port: p-net is GPL, and we want full control.
  - **Native Windows over Npcap**, no WSL.
  - **Npcap licensing:** the free Npcap is fine for development and this PoC on our own PCs (up to 5 systems).
    Shipping on customer machines needs Npcap OEM, and management is still deciding.
  - L2 access sits behind an `L2Port` interface, so a future NDISPROT-based driver backend can replace Npcap without
    touching the stack.
- **Target:**
  - Conformance Class A, **RT_CLASS_1 only**.
  - Send clock 1 ms (SendClockFactor 32). Update time **8–32 ms** (ReductionRatio 8–32; minimum configurable, default 8 ms).
  - The same timing envelope as the EtherNet/IP adapter (RPI ≥ 10 ms).
- **Test controller:** a **CODESYS PROFINET IO-Controller** on separate hardware (CODESYS Control for Linux SL or
  Raspberry Pi SL, demo mode), connected by cable to a dedicated PC NIC.
- **Goal of the PoC:** CODESYS finds the device (DCP), names it, imports its GSDML, connects, and exchanges cyclic I/O
  with `fb_device_demo --transport profinet` for ≥ 1 h at 8/16/32 ms with no AR abort. Measured numbers go to the docs for the go/no-go decision.

### Out of scope
- IRT/RT_CLASS_3, MRP, SNMP/LLDP-MIB, I&M1–4 write support, DHCP, Fast Startup.
- More than one AR, supervisor AR, shared device, system redundancy.
- Applying a *different* IP through DCP. The PoC accepts only the NIC's current IP; see §4.4.
- Certification, PI vendor ID, Dev.3 sync (`tools/sync-to-dev3.ps1`), the NDISPROT driver.

### Protocol references (the IEC spec is paywalled; use these)
- IEC 61158-5-10 / 61158-6-10 (PROFINET IO) and PI's "PROFINET IO Specification": the normative source, where available.
- **Wireshark dissectors**, the readable de-facto reference for every field, block and error code value:
  `plugins/epan/profinet/packet-dcerpc-pn-io.c`, `packet-pn-dcp.c`, `packet-pn-rt.c`, `packet-pn-rtc-one.c`,
  `epan/dissectors/packet-dcerpc.c`, `packet-lldp.c`.
- PI GSDML specification V2.4x plus the PI GSDML Checker.
- p-net (rt-labs) may be **read** to understand behaviour. It is GPL: **copy no code**.
- **Golden captures:** record CODESYS ↔ device traffic with Wireshark as soon as each feature exists, and store small
  `.pcapng` excerpts in `tests/profinet/captures/` for the self-tests.
- Values marked **[verify]** below must be checked against Wireshark/CODESYS captures during implementation, then the doc updated.

---

## 2. Architecture

```
 softfb::FieldbusDevice (Transport::ProfinetRt) ──► softpn::Device (pimpl, public API)
                                                     │
   ┌─────────────────────────────────────────────────┼───────────────────────────────────────────┐
   │ L2Port (interface)   NpcapPort (Win) │ PacketSocketPort (Linux AF_PACKET) │ [future NdisProtPort]
   │ Dcp       Identify/Get/Set/Signal/FactoryReset responder, Hello                               │
   │ Lldp      periodic transmitter                                                               │
   │ RpcServer DCE/RPC connectionless over UDP 34964 (Winsock/BSD sockets), fragment reassembly   │
   │ RpcClient ApplicationReady towards the controller                                            │
   │ Ar        AR state machine + record read/write + Model (slots, submodules, I&M0)             │
   │ Cyclic    provider (input CR TX) + consumer (output CR RX, data-hold watchdog)                │
   │ Rta       alarm protocol (ACK/ERR, retransmission)                                           │
   └──────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Threads
| Thread | Does | Priority |
|---|---|---|
| L2 RX | Blocks on the L2Port receive event. Dispatches by EtherType/FrameID to Dcp, Cyclic consumer, Rta, LLDP (ignored) | TIME_CRITICAL (optional) |
| Cyclic TX | `softeip::PeriodicTimer` at the update time: builds and sends the input-CR frame, checks the data-hold watchdog. Every 100 ms: LLDP TX (every 5 s) and RTA retransmission | TIME_CRITICAL (optional) |
| RPC | UDP socket on 34964, `recvfrom` with a 50 ms timeout. Handles requests and drives AR timers (activity timeout, ApplicationReady retry) | normal |

- **Locking:**
  - `Ar` state is guarded by one `std::mutex`.
  - The process image uses its own mutex, the same pattern as `softeip::Adapter`.
  - The RX → Cyclic consumer copies under the image mutex only.
- **Callbacks** run on the thread that detected the event; document "keep it short", the same as softeip.

### Naming convention (same as `softeip`, from the PLC's view)
| softpn API | Direction | PROFINET | FieldbusDevice area (PC view) |
|---|---|---|---|
| `setInputData()` | PC → PLC | **input** submodule, slot 1 (input CR) | output area (`ioWrite`) |
| `outputData()` | PLC → PC | **output** submodule, slot 2 (output CR) | input area (`ioRead`) |

---

## 3. Repository changes (file map)

### New
```
include/softpn/pn_device.hpp          public API (softpn::Device, DeviceConfig, DeviceState, Stats)
include/softpn/l2_port.hpp            L2Port interface, AdapterInfo, listAdapters(), openL2Port()
src/pn/pn_defs.hpp                    constants: EtherTypes, FrameIDs, DCP options, block types, opnums, UUIDs, error codes
src/pn/pn_frame.hpp/.cpp              Ethernet header (+ optional VLAN) build/parse, MacAddress, padding to 60 bytes
src/pn/l2_port_npcap.cpp              Npcap backend (Windows; compiled only with SOFTPN_NPCAP_SDK)
src/pn/l2_port_linux.cpp              AF_PACKET backend
src/pn/pn_dcp.hpp/.cpp                DCP codec + responder
src/pn/pn_lldp.hpp/.cpp               LLDP frame builder + transmitter
src/pn/pn_station_store.hpp/.cpp      persistence of NameOfStation (+ IP) in an INI file
src/pn/pn_uuid.hpp                    Uuid type, drep-aware encode/decode, PNIO object UUID helper
src/pn/pn_rpc.hpp/.cpp                DCE/RPC CL header codec, NDR args, reassembly, response cache, server + client
src/pn/pn_blocks.hpp/.cpp             PNIO block codec (AR, IOCR, AlarmCR, ExpectedSubmodule, ModuleDiff, Control, Read/Write, I&M0)
src/pn/pn_model.hpp/.cpp              device model: DAP + modules, ident numbers, module variants (shared table with GSDML)
src/pn/pn_ar.hpp/.cpp                 AR state machine (connect, records, PrmEnd, ApplicationReady, release, abort)
src/pn/pn_cyclic.hpp/.cpp             provider + consumer, cycle counter, data status, IOPS/IOCS, watchdog
src/pn/pn_rta.hpp/.cpp                alarm protocol (RTA) ACK/ERR/retransmission
src/pn/pn_device.cpp                  softpn::Device facade wiring all of the above
gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml
examples/pn_device_demo.cpp           echo + heartbeat device, like eip_adapter_demo
tools/pn_probe.cpp                    adapters list, L2 jitter test, DCP identify / set-name / blink (mini Proneta)
tools/pn_controller_sim.cpp           minimal IO-Controller for regression (like eip_scanner_sim)
tools/pn_selftest.cpp                 offline codec tests on golden frames, prints RESULT: PASS/FAIL
tests/profinet/captures/*.pcapng      golden captures (small)
tests/profinet/veth_test.sh           Linux: veth pair + pn_device_demo + pn_controller_sim
docs/phases/profinet/Phase9.NN-*.md   generated by /spec-to-code
docs/phases/phase-9-profinet.md       results summary (like phase-6-modbus.md)
docs/profinet-codesys-setup.md        CODESYS controller + Windows NIC setup guide
```

### Changed
- `include/softeip/bytes.hpp`: add the big-endian helpers PROFINET needs. Additive only; existing callers unchanged.
  - ByteWriter: `u32be`, `patchU16be`, `patchU32be`, `bytesAt`.
  - ByteReader: `u32be`, `pos()`, `seek()`.
- `include/softeip/export.hpp`: add a `SOFTPN_API` macro block. Copy the `SOFTFB_API` pattern, and include it under `SOFTFIELDBUS_SHARED`.
- `CMakeLists.txt`: see §6.
- `include/softfb/fieldbus_device.hpp` / `src/fieldbus_device.cpp`: `Transport::ProfinetRt`, `DeviceConfig::ProfinetOptions`, parse `"profinet"`/`"pn"`.
- `dotnet/SoftFieldbusNet.h/.cpp` + `dotnet/fieldbus_sample`: enum value + options.
- `examples/fb_device_demo.cpp`: `--transport profinet --adapter <name|mac|ip> --station <name>`.
- `README.md`, `docs/PLAN.md` (Phase 9 section), `docs/HANDOFF.md` (Windows steps).

### Reuse (do not re-implement)
| What | Where |
|---|---|
| Byte reader/writer, `ParseError` | `include/softeip/bytes.hpp` |
| High-resolution cycle timer | `include/softeip/periodic_timer.hpp` (`PeriodicTimer`, ms period) |
| Winsock init, `timeBeginPeriod(1)`, power-throttling opt-out | `include/softeip/socket_compat.hpp` (`SocketLibrary`) |
| Export macros | `include/softeip/export.hpp` |
| Transport switch, state mapping pattern | `src/fieldbus_device.cpp` |
| Simulator/test-tool style (CLI args, RESULT: PASS) | `tools/eip_scanner_sim.cpp`, `tools/mb_client_test.cpp` |
| Thread priority + clean stop pattern | `src/eip_adapter.cpp` |

---

## 4. Functional specification

### 4.1 Public API (`include/softpn/pn_device.hpp`)
```cpp
namespace softpn {
enum class DeviceState { Stopped, WaitingForController, Connecting, DataRun, DataStop };
// Connecting = AR established, parameterisation (Connect..ApplicationReady); DataStop = controller provider STOP

struct ModuleVariant { size_t bytes; uint32_t inputModuleIdent; uint32_t outputModuleIdent; }; // table in pn_model.cpp

struct DeviceConfig {
    std::string adapter;            // NPF name, friendly name, MAC "aa:bb:..", or IPv4 of the NIC
    std::string stationName;        // default NameOfStation if none persisted ("" = unnamed)
    std::string persistPath = "softpn.ini";
    uint16_t vendorId = 0xFFFF;     // PLACEHOLDER until Univision has a PI vendor ID – must match GSDML
    uint16_t deviceId = 0x0001;
    std::string orderId = "SOFTPN-1";          // I&M0, max 20 chars
    std::string serialNumber = "0000000000000001"; // I&M0, max 16 chars
    uint16_t hwRevision = 1;
    char swRevPrefix = 'V'; uint8_t swRev[3] = {0, 1, 0};
    std::string deviceVendorValue = "Univision SoftPN"; // DCP TypeOfStation
    size_t inputSize = 32;          // PC -> PLC, must be one of the module variants (16/32/64/128/256)
    size_t outputSize = 32;         // PLC -> PC, idem
    uint32_t minUpdateTimeMs = 8;   // Connect with SCF*RR below this is refused
    bool sendVlanTag = true;        // RT frames tagged prio 6, VID 0
    bool raiseThreadPriority = true;
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>& out, bool run)> onOutputs; // RX thread
    std::function<void(DeviceState)> onStateChanged;
    std::function<void()> onSignal;  // DCP "blink" request
};

struct Stats { uint64_t txFrames, rxFrames, lateCycles, arConnects, arAborts; uint32_t maxTxPeriodUs; std::string lastAbort; };

class SOFTPN_API Device {   // pimpl, no winsock/pcap headers in this file
public:
    explicit Device(DeviceConfig);  ~Device();
    bool start(std::string* error = nullptr); void stop();
    bool setInputData(const void* data, size_t len);   // PC -> PLC, thread safe
    std::vector<uint8_t> outputData() const;           // PLC -> PC, thread safe
    bool controllerRun() const; DeviceState state() const;
    std::string stationName() const; Stats stats() const;
    static std::vector<std::string> listAdapters();    // "name | friendly | mac | ip"
};
}
```

### 4.2 L2 layer (`L2Port`)
```cpp
struct AdapterInfo { std::string name, friendlyName; MacAddress mac; uint32_t ipv4, netmask, gateway; };
class L2Port {
public:
    virtual ~L2Port() = default;
    virtual bool send(const uint8_t* frame, size_t len) = 0;             // thread safe
    virtual bool receive(std::vector<uint8_t>& frame, int timeoutMs) = 0; // RX thread only; false on timeout
    virtual void interrupt() = 0;                                         // wakes receive() for stop()
    virtual const AdapterInfo& info() const = 0;
};
std::vector<AdapterInfo> listAdapters();
std::unique_ptr<L2Port> openL2Port(const std::string& selector, std::string* error);
```
**Npcap backend (`l2_port_npcap.cpp`):**
- **Loading:**
  - Delay-load `wpcap.dll` (`/DELAYLOAD:wpcap.dll`, link `delayimp.lib`). Before the first call, run `SetDllDirectoryW(<System32>\Npcap)`.
  - If loading fails, return the error "Npcap is not installed (https://npcap.com)". Never crash.
- **Adapter selection:**
  - Enumerate with `pcap_findalldevs`, and match `\Device\NPF_{GUID}` with `GetAdaptersAddresses` (by GUID) to get the MAC, IPv4, mask and gateway.
  - The selector matches the NPF name, the friendly name, the MAC or the IPv4.
- **Two pcap handles:** one RX and one TX, so sends need no lock against the RX loop. TX is still guarded by a mutex,
  because DCP replies go out from the RX thread while the cyclic thread also sends.
- **RX handle:**
  - `pcap_create` → `pcap_set_snaplen(1522)`, `pcap_set_promisc(0)`, `pcap_set_immediate_mode(1)`,
    `pcap_set_buffer_size(4 MB)`, `pcap_set_timeout(1)` → `pcap_activate`.
  - BPF: `(ether proto 0x8892 or ether proto 0x88cc or (vlan and ether proto 0x8892)) and not ether src <own MAC>`.
  - Wait on `pcap_getevent()` plus an interrupt event (`WaitForMultipleObjects`), then drain with `pcap_next_ex`.
- **Multicast reception** of DCP Identify `01:0E:CF:00:00:00`: with promisc off, the NIC filter may drop it **[verify]**.
  If it does, enable promiscuous mode on the RX handle (the BPF still filters).
- **Frames:** pad TX frames to 60 bytes.
- **NIC caveat:** the Intel "Packet Priority & VLAN" driver setting can strip VLAN tags on RX. The parser accepts both
  tagged and untagged frames. Document it in the setup guide.

**Linux backend (`l2_port_linux.cpp`):**
- `socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))`, bound to the ifindex.
- `PACKET_ADD_MEMBERSHIP` for `01:0E:CF:00:00:00` and the LLDP multicast `01:80:C2:00:00:0E`.
- Filter in user space: drop our own source MAC, keep only 0x8892/0x88CC.
- `poll()` with an eventfd for `interrupt()`. Needs `CAP_NET_RAW`.
- Adapter info from `getifaddrs` + `SIOCGIFHWADDR`.

### 4.3 Frame basics (`pn_frame`)
- Ethernet II: dst(6) src(6) [0x8100 TCI(2)] EtherType(2) payload.
  - PROFINET EtherType **0x8892**, LLDP **0x88CC**.
  - TX VLAN TCI for RT/RTA = priority 6, VID 0 → `0xC000` (when `sendVlanTag`).
- **FrameID** (2 bytes, big-endian) right after the EtherType:
  - `0x8000–0xBFFF` RT_CLASS_1 cyclic. The value comes from the controller's IOCRBlockReq.
  - `0xFC01` alarm high, `0xFE01` alarm low.
  - `0xFEFC` DCP Hello, `0xFEFD` DCP Get/Set, `0xFEFE` DCP Identify request (multicast), `0xFEFF` DCP Identify response.

### 4.4 DCP (`pn_dcp`)
- **Header** after the FrameID: ServiceID(1), ServiceType(1), Xid(4), ResponseDelay(2), DCPDataLength(2), then blocks.
  - Block: Option(1), Suboption(1), DCPBlockLength(2), payload. Responses carry BlockInfo(2) before the data;
    Set requests carry BlockQualifier(2). Each block is padded to an even length.
  - ServiceID: 3 Get, 4 Set, 5 Identify, 6 Hello.
  - ServiceType: 0 request, 1 response success, 5 response "not supported" **[verify]**.
- **Options supported:**
  | Option/Sub | Name | Get | Set | In Identify response |
  |---|---|---|---|---|
  | 1/1 | MAC address | ✓ | – | – |
  | 1/2 | IP parameter (ip, mask, gw); BlockInfo bit0 "IP set" | ✓ | ✓ (see IP rule) | ✓ |
  | 2/1 | DeviceVendorValue (TypeOfStation) | ✓ | – | ✓ |
  | 2/2 | NameOfStation | ✓ | ✓ | ✓ |
  | 2/3 | DeviceID (VendorID, DeviceID) | ✓ | – | ✓ |
  | 2/4 | DeviceRole (0x01 IO-Device, 1 reserved byte) | ✓ | – | ✓ |
  | 2/5 | DeviceOptions (list of supported option/suboption pairs) | ✓ | – | ✓ |
  | 2/7 | DeviceInstance (0x00, 0x01) | ✓ | – | ✓ |
  | 5/3 | Control Signal (value 0x0100 → `onSignal`) | – | ✓ | – |
  | 5/5, 5/6 | FactoryReset / ResetToFactory → clear name (and IP-set flag) | – | ✓ | – |
  | 0xFF/0xFF | All selector (Identify filter) | – | – | – |
- **Identify:**
  - Respond only if the request filter matches: the All selector, or NameOfStation equal (case-insensitive), or the
    DeviceID / alias options when present.
  - The response goes unicast to the requester's MAC with FrameID 0xFEFF and the same Xid.
  - If ResponseDelay > 1: wait `(hash(own MAC) % ResponseDelay) × 10 ms`, scheduled on the cyclic thread's housekeeping, never sleeping in RX.
- **Set response:** one Control/Response block (5/4) per Set block: Option, Suboption, BlockError.
  - BlockError: 0 ok, 1 option unsupported, 2 suboption unsupported, 3 not set, 4 resource, 5 local reasons, 6 in operation **[verify]**.
- **NameOfStation validation:**
  - Lowercase DNS label rules: total ≤ 240 characters, each label ≤ 63, `[a-z0-9-]`, no leading or trailing `-`.
  - Not of the form `port-xyz` / `port-xyz-abcde`, and not an IPv4 dotted quad.
  - An empty name is allowed (= reset).
  - BlockQualifier 1 (permanent) → persist. 0 (temporary) → keep in RAM only.
- **IP rule (PoC):**
  - A Set IP equal to the NIC's current IP/mask: answer OK, and set the IP-set flag.
  - Anything else: BlockError 5. Log "configure the controller with the PC's static IP a.b.c.d".
  - Set Name/IP while an AR is in Data: BlockError 6 **[verify]**.
- **Hello** (FrameID 0xFEFC): not required for the PoC. Implement only the codec.

### 4.5 LLDP (`pn_lldp`)
- Destination `01:80:C2:00:00:0E`, EtherType 0x88CC, sent every 5 s with TTL 20 s.
- **TLVs:**
  - Chassis ID: subtype 7 "locally assigned" = NameOfStation, or subtype 4 MAC if unnamed.
  - Port ID: subtype 7 = `port-001` **[verify]** the format CODESYS expects.
  - TTL 20.
  - IEEE 802.3 MAC/PHY config (OUI 00-12-0F, subtype 1): auto-negotiation supported/enabled, operational MAU type
    (100BASE-TX FD = 16), taken from the adapter link speed when available.
  - PROFINET org TLVs (OUI 00-0E-CF): subtype 2 PortStatus (RTClass2/3 = 0), subtype 5 ChassisMAC.
  - End.
- Received LLDP is ignored in the PoC.
- Setup guide: unbind "Microsoft LLDP Protocol Driver" from the NIC.

### 4.6 DCE/RPC connectionless (`pn_rpc`, `pn_uuid`)
- **Transport:** UDP port **34964 (0x8894)**, through the OS IP stack (`softeip::SocketLibrary` on Windows).
- **80-byte header:**
  - rpc_vers=4, ptype, flags1, flags2, drep[3], serial_hi.
  - ObjectUUID(16), InterfaceUUID(16), ActivityUUID(16).
  - server_boot(4), interface_vers(4), seqnum(4), opnum(2), ihint(2), ahint(2), len(2), fragnum(2), auth_proto(1), serial_lo(1).
- **Endianness:** `drep[0]` bit4 = 1 → little-endian. Numeric header and NDR fields follow drep, and so do UUID
  fields 1–3 (`Data1/2/3`). The PNIO blocks inside are **always big-endian**.
- **ptype:** 0 request, 1 ping, 2 response, 3 fault, 4 working, 5 nocall, 6 reject, 7 ack, 9 fack.
- **flags1:** 0x02 last fragment, 0x04 fragment, 0x08 no-fack, 0x20 idempotent.
- **UUIDs:**
  - PNIO Device interface `DEA00001-6C97-11D1-8271-00A02442DF7D`, Controller interface `DEA00002-…`, version 1.
  - Device object UUID `DEA00000-6C97-11D1-8271-<InstanceHi InstanceLo DeviceIdHi DeviceIdLo VendorIdHi VendorIdLo>` with instance 0x0001.
- **Opnums:** 0 Connect, 1 Release, 2 Read, 3 Write, 4 Control, 5 ReadImplicit.
- **NDR request args:** ArgsMaximum(4), ArgsLength(4), MaximumCount(4), Offset(4), ActualCount(4), blocks.
- **NDR response args:** PNIOStatus(4: ErrorCode, ErrorDecode, ErrorCode1, ErrorCode2), ArgsLength(4), MaximumCount(4),
  Offset(4), ActualCount(4), blocks. MaximumCount = the request's ArgsMaximum.
- **Server behaviour:**
  - Reassemble fragmented requests by (activity, seqnum, fragnum). Send fack only if no-fack is clear.
  - Fragment responses larger than 1392 bytes of body.
  - Cache the last response per activity: a duplicate seqnum gets the cached response again; ping gets `working` or the cached response.
  - Unknown interface or opnum → `reject` (or a fault with nca_unk_if / nca_op_rng_error **[verify]**).
  - The EPM (endpoint mapper, interface E1AF8308-5D1F-11C9-91A4-08002B14A0FA) is optional. Answer it with reject in the PoC.
- **Client (ApplicationReady):**
  - Sends request opnum 4 on the Controller interface to the controller IP and the `CMInitiatorUDPRTPort` from ARBlockReq,
    with object UUID = `CMInitiatorObjectUUID` and a new activity UUID.
  - Retries every 1 s, up to the activity timeout.

### 4.7 PNIO blocks (`pn_blocks`)
- **Block header:** BlockType(2), BlockLength(2, excludes type+length), VersionHigh(1)=1, VersionLow(1)=0.

| Type | Block | Direction | Key fields (in order) |
|---|---|---|---|
| 0x0101/0x8101 | ARBlockReq/Res | C→D / D→C | Req: ARType(2), ARUUID(16), SessionKey(2), CMInitiatorMacAdd(6), CMInitiatorObjectUUID(16), ARProperties(4), CMInitiatorActivityTimeoutFactor(2, ×100 ms), CMInitiatorUDPRTPort(2), StationNameLength(2), CMInitiatorStationName. Res: ARType, ARUUID, SessionKey, CMResponderMacAdd(6), CMResponderUDPRTPort(2)=0x8894 |
| 0x0102/0x8102 | IOCRBlockReq/Res | | Req: IOCRType(2: 1 input, 2 output), IOCRReference(2), LT(2)=0x8892, IOCRProperties(4: bits 0–3 RTClass; accept 1 and 2 **[verify]**), DataLength(2), FrameID(2), SendClockFactor(2), ReductionRatio(2), Phase(2), Sequence(2), FrameSendOffset(4), WatchdogFactor(2), DataHoldFactor(2), IOCRTagHeader(2), IOCRMulticastMACAdd(6), NumberOfAPIs(2) × { API(4), NumberOfIODataObjects(2) × {Slot(2), Subslot(2), FrameOffset(2)}, NumberOfIOCS(2) × {Slot(2), Subslot(2), FrameOffset(2)} }. Res: IOCRType, IOCRReference, FrameID |
| 0x0103/0x8103 | AlarmCRBlockReq/Res | | Req: AlarmCRType(2)=1, LT(2), AlarmCRProperties(4), RTATimeoutFactor(2, ×100 ms), RTARetries(2), LocalAlarmReference(2), MaxAlarmDataLength(2), TagHeaderHigh(2), TagHeaderLow(2). Res: AlarmCRType, LocalAlarmReference (ours), MaxAlarmDataLength |
| 0x0104 | ExpectedSubmoduleBlockReq | C→D | NumberOfAPIs(2) × { API(4), Slot(2), ModuleIdentNumber(4), ModuleProperties(2), NumberOfSubmodules(2) × { Subslot(2), SubmoduleIdentNumber(4), SubmoduleProperties(2: bits 0–1 type 0 none/1 in/2 out/3 in+out), DataDescription × {Type(2: 1 in, 2 out), SubmoduleDataLength(2), LengthIOCS(1), LengthIOPS(1)} } }. May appear several times |
| 0x8104 | ModuleDiffBlock | D→C | Only if anything differs: NumberOfAPIs × { API, NumberOfModules × { Slot, ModuleIdentNumber, ModuleState(2: 0 none, 1 wrong, 2 proper, 3 substitute), NumberOfSubmodules × { Subslot, SubmoduleIdentNumber, SubmoduleState(2) } } } |
| 0x8106 | ARServerBlock | D→C | StationNameLength(2), CMResponderStationName, padding to a 4-byte boundary. Sent when ARProperties.StartupMode = Advanced **[verify]** |
| 0x0110/0x8110 | IODControlReq/Res (PrmEnd) | | Reserved(2), ARUUID(16), SessionKey(2), Reserved(2), ControlCommand(2), ControlBlockProperties(2). Commands: 0x0001 PrmEnd, 0x0002 ApplicationReady, 0x0004 Release, 0x0008 Done, 0x0040 PrmBegin |
| 0x0112/0x8112 | IOXBlockReq/Res (ApplicationReady) | D→C / C→D | same layout as 0x0110 |
| 0x0114/0x8114 | IODReleaseBlock Req/Res | | same layout; Res ControlCommand = Done |
| 0x0008/0x8008 | IODWriteReqHeader/ResHeader | | Req: SeqNumber(2), ARUUID(16), API(4), Slot(2), Subslot(2), Padding(2), Index(2), RecordDataLength(4), Padding(24), data. Res: … Index, RecordDataLength, AdditionalValue1(2), AdditionalValue2(2), PNIOStatus(4), Padding(16) |
| 0x0009/0x8009 | IODReadReqHeader/ResHeader | | Req: same as Write minus data (ReadImplicit adds TargetARUUID). Res: … Index, RecordDataLength(4), AdditionalValue1(2), AdditionalValue2(2), Padding(20), data |
| 0x0020 | I&M0 | | VendorID(2), OrderID(20, space-padded), SerialNumber(16), HardwareRevision(2), SoftwareRevision(1 char + 3 bytes), RevisionCounter(2), ProfileID(2)=0, ProfileSpecificType(2)=0, IM_Version(2)=0x0101, IM_Supported(2)=0 |

- Every parser bounds-checks through `ByteReader` (throws `ParseError` → RPC error response, never a crash).
- Every builder patches BlockLength at the end (`patchU16be`).

**PNIOStatus error codes:**
- ErrorCode: 0xDB Connect, 0xDC Release, 0xDD Control, 0xDE Read, 0xDF Write.
- ErrorDecode: 0x80 PNIORW (record access) / 0x81 PNIO.
- ErrorCode1 for Connect, by block: 0x01 ARBlockReq, 0x02 IOCRBlockReq, 0x03 ExpectedSubmoduleBlockReq, 0x04 AlarmCRBlockReq, 0x40 CMRPC (resources / state).
- ErrorCode2 = the faulty field index.
- Record errors (ErrorDecode 0x80): ErrorCode1 0xB0 invalid index, 0xB2 invalid slot/subslot, 0xB6 access denied.
- **[verify] all values** against the Wireshark value strings, and keep them in one table in `pn_defs.hpp`.

### 4.8 Device model (`pn_model`)
| Slot | Subslot | What | Module / Submodule ident | Data |
|---|---|---|---|---|
| 0 | 0x0001 | DAP | Module 0x00000001 / Sub 0x00000001 | none (IOPS in input CR, IOCS in output CR) |
| 0 | 0x8000 | Interface | Sub 0x00008000 | none |
| 0 | 0x8001 | Port 1 | Sub 0x00008001 | none |
| 1 | 0x0001 | Input module N bytes (PC → PLC) | Module 0x0000001x / Sub 0x00000001 | `inputSize` bytes + IOPS |
| 2 | 0x0001 | Output module N bytes (PLC → PC) | Module 0x0000002x / Sub 0x00000001 | `outputSize` bytes, IOCS in input CR |

- **Variant table:** 16/32/64/128/256 bytes → input idents 0x10–0x14, output idents 0x20–0x24.
  The table lives once in `pn_model.cpp`; the GSDML must use the same numbers (the self-test checks them by parsing the XML).
- **Connect check:**
  - For each expected submodule, compare it with the real one: proper / wrong / no module.
  - Wrong submodules get IOPS BAD (0x00) in the cyclic frame. Proper ones get GOOD (0x80) once the AR reaches Data.
- **ModuleDiffBlock** lists only the differences.

### 4.9 AR state machine (`pn_ar`)
```
 Idle ──Connect.req ok──► WaitPrmEnd ──Control(PrmEnd)──► (reply Done) ──► WaitAppReadyCnf
   ▲                         │  Write.req (records)                          │ send ApplicationReady (RPC client, retry 1 s)
   │                         │                                               ▼ Control.res(Done) from controller
   └──── Release / abort ◄───┴──────────────────────────────────────────── Data
```
- **One AR only.** A second Connect while busy → PNIOStatus 0xDB 0x81 0x40 (CMRPC, out of AR resources) **[verify]**.
  - A Connect with the **same ARUUID** as the active AR: treat it as a re-connect. Abort the old AR, then accept.
- **Connect validation:**
  - ARType 0x0001 (IOCARSingle) only.
  - Exactly one input and one output IOCR.
  - RTClass RT_CLASS_1, SendClockFactor == 32, ReductionRatio a power of 2 with SCF×RR×31.25 µs ≥ `minUpdateTimeMs`.
  - DataLength 40…1440, and every FrameOffset + length ≤ DataLength.
  - One AlarmCR.
  - Each failure → the matching PNIOStatus with the field index.
- **Connect.res:** ARBlockRes, IOCRBlockRes ×2, AlarmCRBlockRes, [ModuleDiffBlock], [ARServerBlock].
  Start the cyclic provider immediately after sending it (IOPS BAD until Data).
- **Write:**
  - Accept writes on slot 0 subslots 0x8000/0x8001: PDev records such as 0x802B PDPortDataCheck and 0x802F PDPortDataAdjust.
    Store and ignore them, and answer OK.
  - **Index 0xE040 (multiple write):** iterate the contained write headers and answer with a multiple-write response.
  - Anything else → PNIORW invalid index.
- **Read / ReadImplicit:**
  - 0xAFF0 I&M0 on any existing subslot → the I&M0 block.
  - 0xF840 I&M0FilterData **[verify]** if CODESYS asks for it.
  - Anything else → invalid index (reads must never kill the AR).
- **Timers:**
  - Activity timeout: if no PrmEnd within `CMInitiatorActivityTimeoutFactor × 100 ms` after Connect → abort.
  - ApplicationReady: retry every 1 s until the activity timeout.
- **Abort** (any cause: consumer DHT expired, RTA ERR received, release, timeout, stop()):
  - Stop the provider, send RTA ERR if the AlarmCR is up, go to Idle, set `onOutputs(zeros, false)` semantics
    (outputs invalid), and increment `stats.arAborts`.
- **State mapping:** Idle → WaitingForController; WaitPrmEnd/WaitAppReadyCnf → Connecting; Data + controller provider RUN → DataRun; Data + STOP → DataStop.

### 4.10 Cyclic RT (`pn_cyclic`)
- **Frame:** Ethernet [VLAN] 0x8892, FrameID, C_SDU (DataLength bytes), CycleCounter(2 BE), DataStatus(1), TransferStatus(1)=0.
- **Provider** (input CR, device → controller), every SCF×RR×31.25 µs (= RR ms):
  - Zero the C_SDU, then place each IODataObject at its FrameOffset: the slot 1 data (`setInputData` buffer) followed by its
    IOPS. The DAP submodules have only an IOPS byte at their offset.
  - Place the IOCS for the output submodules (slot 2 and the DAP ones) at the IOCS offsets.
  - CycleCounter += SCF × RR per frame (31.25 µs units).
  - DataStatus `0x35` = State primary (bit0), DataValid (bit2), ProviderState RUN (bit4), StationProblemIndicator normal (bit5).
    In DataStop/Connecting it stays `0x35` with IOPS BAD on the IO submodules **[verify]** what CODESYS expects before ApplicationReady.
  - Track the actual period with QPC/steady_clock: `maxTxPeriodUs`, `lateCycles` (period > 1.5 × nominal).
- **Consumer** (output CR, controller → device) on the RX thread:
  - Match the FrameID and source MAC = CMInitiatorMacAdd. Check the length.
  - Read DataStatus: bit4 ProviderState → run/stop; bit2 DataValid.
  - Read slot 2's IOPS. Copy the slot 2 data into the output image when IOPS GOOD and DataValid; else mark the outputs invalid.
  - Call `onOutputs(data, run)` when the data or run changed.
  - Accept the frame only when the CycleCounter moved forward (ignore duplicates).
- **Data-hold watchdog:** if no valid frame for `DataHoldFactor × SCF × RR × 31.25 µs`, abort the AR with the consumer DHT expired reason **[verify the code]**.
  - Grace period after Connect: the watchdog starts with the first received frame or at ApplicationReady, whichever comes first **[verify]**.

### 4.11 Alarms, RTA (`pn_rta`)
- **Frame:** FrameID 0xFC01 (high) / 0xFE01 (low), then the RTA-PDU:
  AlarmDstEndpoint(2), AlarmSrcEndpoint(2), PDUType(1: version 1 in the high nibble, type in the low nibble: 1 DATA, 2 NACK, 3 ACK, 4 ERR),
  AddFlags(1: WindowSize=1, bit4 TACK), SendSeqNum(2), AckSeqNum(2), VarPartLen(2), VarPart.
- **Endpoints:** our LocalAlarmReference ↔ the controller's LocalAlarmReference from AlarmCRBlockReq.
- **PoC scope:**
  - Receiving DATA → reply ACK (sequence handling per spec; initial values **[verify]** from a capture).
  - Receiving ERR → abort the AR.
  - On local abort → send ERR with the PNIOStatus.
  - Retransmit our own DATA/ERR every `RTATimeoutFactor × 100 ms`, up to RTARetries.
  - API hook `sendProcessAlarm()` is stubbed (not in the PoC).

### 4.12 Persistence (`pn_station_store`)
- INI file at `persistPath`: `[station] name=… ipSet=0|1`.
- Write atomically: write a temp file, then rename (`MoveFileExW(MOVEFILE_REPLACE_EXISTING)` / `rename`).
- Load in `start()`; the config `stationName` is used only if the file has no name.

---

## 5. GSDML (`gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml`)
- ProfileHeader (PROFINET Device Profile, ISO 15745).
- DeviceIdentity: VendorID = the **placeholder 0xFFFF** (must equal `DeviceConfig::vendorId`), DeviceID 0x0001, InfoText, VendorName "Univision".
- DeviceFunction: Family MainFamily "I/O", ProductFamily "SoftPN".
- **DeviceAccessPointItem "DAP1":**
  - PhysicalSlots "0..2", ModuleIdentNumber 0x00000001, MinDeviceInterval 32, ImplementationType "Software",
    DNS_CompatibleName "softpn-device", FixedInSlots 0, ObjectUUID_LocalIndex 1, RequiredSchemaVersion "V2.43", PNIO_Version "V2.43",
    ConformanceClass "A", NumberOfDeviceAccessAR 1, DeviceAccessSupported false.
  - ModuleInfo: Name, OrderNumber "SOFTPN-1", HardwareRelease, SoftwareRelease.
  - IOConfigData MaxInputLength/MaxOutputLength 1440.
  - UseableModules: input modules AllowedInSlots "1", output modules AllowedInSlots "2".
  - VirtualSubmoduleList: subslot 1, ident 0x00000001, no IO.
  - SystemDefinedSubmoduleList:
    - InterfaceSubmoduleItem subslot 32768, ident 0x8000, SupportedRT_Classes "RT_CLASS_1",
      ApplicationRelations StartupMode "Advanced;Legacy" **[verify]**,
      TimingProperties SendClock "32", ReductionRatio "8 16 32 64 128 256 512" (matches `minUpdateTimeMs` 8).
    - PortSubmoduleItem subslot 32769, ident 0x8001, MAUTypes "16", PortDeactivationSupported false.
  - Graphics optional (omit for the PoC).
- **ModuleList:**
  - 5 input modules "Input N bytes" (idents 0x10–0x14): VirtualSubmoduleItem with IOData Input DataItem DataType "OctetString" Length N.
  - 5 output modules "Output N bytes" (idents 0x20–0x24) the same, with IOData Output.
- ExternalTextList, PrimaryLanguage English.
- **Validate with the PI GSDML Checker** and by importing into CODESYS. Keep a note of the checker warnings in the phase doc.

---

## 6. Build (`CMakeLists.txt`)
- `set(SOFTPN_NPCAP_SDK "" CACHE PATH "Npcap SDK folder (Include/, Lib/x64/); empty = no PROFINET on Windows")`.
- **The `softpn` STATIC library is built when:** Linux (always), or Windows with `SOFTPN_NPCAP_SDK` set and
  `${SOFTPN_NPCAP_SDK}/Include/pcap.h` present. Otherwise print a message and skip; the other targets stay unchanged.
- **Windows:**
  - Include `${SOFTPN_NPCAP_SDK}/Include`, link `${SOFTPN_NPCAP_SDK}/Lib/x64/wpcap.lib Packet.lib iphlpapi ws2_32 winmm delayimp`.
  - Link option `/DELAYLOAD:wpcap.dll /DELAYLOAD:Packet.dll` (PUBLIC so executables inherit it).
- `target_link_libraries(softpn PUBLIC softeip)` (for the bytes, timer and socket helpers). Apply `softeip_warnings(softpn)`.
- Executables: `pn_device_demo`, `pn_probe`, `pn_controller_sim`, `pn_selftest`.
- Set `FOLDER` properties as for the existing targets, plus `VS_DEBUGGER_COMMAND_ARGUMENTS`.
- `softfieldbus`: if `TARGET softpn`, link it and define `SOFTFB_HAS_PROFINET=1` PUBLIC.
- `SOFTFIELDBUS_SHARED`: append the `src/pn/*.cpp` sources plus the same links/define when softpn is enabled.
- Add `CMakePresets.json` cache variable hints: `SOFTPN_NPCAP_SDK` default `$env{NPCAP_SDK}`.

---

## 7. Phases (for `/spec-to-code`, one phase doc each, ~30–60 min)

Each **gate** is a stop point: record the measured results in `docs/phases/phase-9-profinet.md` before continuing.

### Milestone A – L2 access and timing
| Phase | Title | Content | Done when |
|---|---|---|---|
| 9.01 | BuildScaffolding | CMake option + softpn target + empty `pn_device.cpp`; `SOFTPN_API`; `pn_defs.hpp` constants; bytes.hpp BE helpers | builds on Win (with/without SDK) and Linux |
| 9.02 | L2PortNpcap | `l2_port.hpp`, `pn_frame`, Npcap backend (delay-load, adapter matching, 2 handles, BPF, event-based RX, interrupt) | `pn_probe --list` shows NICs with MAC/IP |
| 9.03 | L2PortLinux | AF_PACKET backend, multicast membership, eventfd interrupt | `pn_probe --list` on Linux |
| 9.04 | ProbeJitter | `pn_probe --send-test`, `--recv-test`, `--jitter --period-ms 1/2/4/8 --seconds 60` (TX period stats via QPC: min/avg/max/p99, histogram); `--rx-jitter` on a second machine/NIC | **Gate A:** p99 TX period error < 0.5 ms at 8 ms and max < 2 ms, on the dedicated NIC under normal desktop load. Numbers recorded |

### Milestone B – Discovery (DCP + LLDP)
| 9.05 | DcpCodec | DCP header/block parse + build, option table, NameOfStation validator; self-test cases | `pn_selftest` dcp cases pass |
| 9.06 | DcpResponder | Identify (filter, delay), Get, Set (name/IP rule/signal/factory reset), station store, responder wired to the RX thread (temporary harness in `pn_device_demo`) | Wireshark shows a correct Identify response |
| 9.07 | Lldp | LLDP builder + 5 s transmitter | Wireshark LLDP dissector clean |
| 9.08 | ProbeDcpTools | `pn_probe identify [--name n]`, `set-name <mac> <name> [--temp]`, `blink <mac>` (DCP controller side); Linux veth smoke test | **Gate B:** CODESYS "Scan for devices" and Proneta (optional) find the device; set-name persists across a restart; blink triggers `onSignal` |

### Milestone C – GSDML, RPC, AR setup
| 9.09 | ModelAndGsdml | `pn_model` variant table + GSDML file; pn_selftest checks the GSDML idents against the table | GSDML Checker passes (warnings noted); CODESYS imports it |
| 9.10 | RpcCodec | `pn_uuid`, RPC header (drep), NDR args, fragment reassembly, response cache; self-test on a captured Connect | selftest passes |
| 9.11 | RpcServer | UDP 34964 server thread, interface/opnum dispatch, reject unknown, ping/working | Wireshark shows a well-formed reject for an unknown opnum |
| 9.12 | BlockCodec | `pn_blocks` parse/build for all §4.7 blocks; ModuleDiff computation; error code table | selftest on captured Connect.req / built Connect.res |
| 9.13 | ArConnect | AR state machine: Connect validation + response, Release, single-AR rule, activity timeout, abort path | CODESYS Connect.res accepted (visible in Wireshark), then timeout/abort is expected |
| 9.14 | RecordReadWrite | Write (incl. 0xE040 multiple write, PDev accept), Read/ReadImplicit I&M0 0xAFF0, invalid index errors | CODESYS parameterisation passes the Write phase; I&M0 is readable |
| 9.15 | PrmEndAppReady | PrmEnd → Done, RPC client ApplicationReady with retries, CControl confirmation → Data | **Gate C:** AR reaches Data in the device log (cyclic may still be missing → DHT abort on the controller side is expected) |

### Milestone D – Cyclic exchange and alarms
| 9.16 | CyclicProvider | Input-CR frame builder from IOCR offsets, cycle counter, data status, IOPS/IOCS, TX thread on PeriodicTimer, stats | Wireshark PN-RT shows a steady cadence and correct IOPS |
| 9.17 | CyclicConsumer | Output-CR parse, run/stop, IOPS, output image + `onOutputs`, data-hold watchdog → abort | CODESYS outputs appear in the device |
| 9.18 | RtaAlarms | RTA ACK/ERR, retransmission, abort integration | Controller-initiated abort and device-initiated abort both clean (Wireshark) |
| 9.19 | DeviceFacade | `softpn::Device` wiring, states, callbacks, stop() under traffic, thread priorities; `pn_device_demo` (echo slot 2 → slot 1 from byte 4, heartbeat counter in bytes 0–3, like eip_adapter_demo) | **Gate D:** CODESYS shows the device green at 8/16/32 ms; echo OK; 1 h soak with 0 AR aborts; STOP/RUN of the PLC → DataStop/DataRun; cable pull → abort + automatic reconnect |

### Milestone E – Integration into the common API
| 9.20 | FieldbusDeviceTransport | `Transport::ProfinetRt`, `ProfinetOptions {adapter, stationName, persistPath, vendorId, deviceId, minUpdateTimeMs, sendVlanTag}`, mapping (output area → `setInputData`, input area ← `outputData`), state mapping (WaitingForMaster, ConnectedIdle = DataStop, ConnectedRun = DataRun, Connecting → WaitingForMaster), `parseTransport("profinet"/"pn")`, guarded by `SOFTFB_HAS_PROFINET` (start() returns an error otherwise); `fb_device_demo` options | `fb_device_demo --transport profinet` passes the Gate D echo test |
| 9.21 | DotNetWrapper | `FieldbusTransport.ProfinetRt` + options in `SoftFieldbus.Net`; C# sample transport switch | C# sample exchanges data with CODESYS |

### Milestone F – Regression and docs
| 9.22 | ControllerSim | `pn_controller_sim`: DCP identify by name (+ optional set-name), Connect from CLI args (sizes, RR), Write PDev dummy, PrmEnd, answers ApplicationReady (RPC server on the controller side), cyclic echo check, Release; `--skip-release`, `--rr 4` (expect refusal), `--wrong-module` (expect ModuleDiff) | On Linux veth (`tests/profinet/veth_test.sh`) and on Windows (same NIC via Npcap TX loopback with a locally-administered sim MAC **[verify]**, otherwise two NICs): RESULT: PASS for all cases |
| 9.23 | SelfTestGolden | `pn_selftest` final: all codecs against `tests/profinet/captures`, GSDML ident check; `enable_testing()` + `add_test(pn_selftest)` | `ctest` passes on Win/Linux |
| 9.24 | Docs | README (protocol table row "PROFINET RT device: implemented PoC", usage, limits, Npcap license note), PLAN.md Phase 9, HANDOFF Windows steps, `docs/profinet-codesys-setup.md`, `phase-9-profinet.md` results tables | docs reviewed with `/spec-audit` |

---

## 8. Windows setup (content for `docs/profinet-codesys-setup.md`)
- **Npcap:** install the free edition from npcap.com. "WinPcap API-compatible mode" is not needed (we load from `System32\Npcap`).
  Dev/PoC only, at most 5 systems; OEM is needed for product redistribution.
- **Dedicated NIC:**
  - Static IP, the same IP configured for the device in CODESYS.
  - Disable: "Microsoft LLDP Protocol Driver" binding, energy-efficient Ethernet/power saving, interrupt moderation.
  - Intel: "Packet Priority & VLAN" set so tags are not stripped (or disabled).
- **Firewall:** allow inbound **UDP 34964** for the executable. DCP/RT/LLDP are Layer 2 and are not affected by the firewall.
- **CODESYS:**
  - Install the GSDML in the Device Repository.
  - Add Ethernet adapter → PROFINET IO Controller → SoftPN device. Set the station name, the IP (= the PC NIC IP) and the
    update time ≥ 8 ms. Use "Scan for devices" to assign the name.
- **Rights:** Npcap normally works without admin (unless installed with "restrict to administrators"). Admin is not needed for the PoC.

---

## 9. Verification (end-to-end)
1. **Build:**
   - `cmake --preset vs2026 -DSOFTPN_NPCAP_SDK=<path>` + build Release/Debug, 0 warnings.
   - `vs2026-analyze` clean.
   - Linux preset clean.
   - A build without the SDK still produces all existing targets.
2. **Offline:** `pn_selftest` → RESULT: PASS (codecs on golden captures, GSDML idents).
3. **Timing:** `pn_probe --jitter` results table at 1/2/4/8 ms (Gate A).
4. **Discovery:** CODESYS scan + set name; `pn_probe identify/set-name/blink`; Wireshark DCP/LLDP dissectors show no malformed packets.
5. **Connection:** CODESYS → device green at 8/16/32 ms; Wireshark PNIO-CM sequence Connect → Write → PrmEnd → ApplicationReady → cyclic.
6. **Soak:** 1 h echo at 8 ms with `fb_device_demo --transport profinet`. Record `stats()` (maxTxPeriodUs, lateCycles, arAborts = 0).
7. **Robustness:** PLC STOP/RUN, controller download/reconnect, cable pull/replug, `stop()` under traffic, second controller
   Connect refused, wrong module → ModuleDiff + IOPS BAD.
8. **Regression:** `pn_controller_sim` all cases PASS on Linux veth and Windows.
9. **Existing tools unchanged:** `eip_scanner_sim` and `mb_client_test` still PASS (bytes.hpp change is additive).

## 10. Risks / open points
- **Npcap OEM license:** management decision before any product use. The L2Port design keeps an NDISPROT backend possible.
- **Windows jitter** under load (antivirus, UI). Gate A decides early, and the minimum update time stays configurable.
- **The [verify] items** (DCP error codes, RT class value, startup mode, RTA initial sequence numbers, DHT abort code) are resolved
  from captures in the phase where they appear.
- **Placeholder VendorID 0xFFFF:** fine in the lab, never in the field.
- **No certification:** the PoC lacks SNMP, I&M1–4, a separate port MAC and the netload robustness test.
