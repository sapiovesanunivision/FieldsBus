# FieldsBus: software fieldbus device for Windows (C++)

Software-only fieldbus **devices** ("slaves") for a standard Windows PC NIC, all behind one
**Hilscher-style process-image API** (see [One API for every fieldbus](#one-api-for-every-fieldbus)):
- **EtherNet/IP adapter** (`softeip`)
- **Modbus TCP / Modbus UDP slave** (`softmb`)

The EtherNet/IP adapter is described first; the Modbus slave and the common API follow.

A software-only **EtherNet/IP adapter** (I/O device / "slave") that runs on a standard Windows PC NIC.
It replaces a Hilscher netX/cifX card where cycle times of roughly 10 ms or more are good enough.
The PLC stays the master (scanner): it opens the connection and exchanges cyclic I/O with the PC.

```
 PLC (scanner / master)                         Windows PC (this library)
 ───────────────────────                        ────────────────────────────────
 TCP 44818  RegisterSession, Forward_Open  ───► encapsulation + CIP objects
 UDP 2222   O->T outputs every RPI         ───► Adapter::outputData()
 UDP 2222   T->O inputs  every RPI         ◄─── Adapter::setInputData()
```

## How hard is a software-only solution?

| Protocol (device side) | Difficulty | Why |
|---|---|---|
| **EtherNet/IP adapter** (this repo) | Easy–moderate | Plain TCP/UDP sockets. No driver, no admin rights, no raw Ethernet. |
| PROFINET RT device | Hard | Raw Layer-2 frames (EtherType 0x8892) through Npcap, plus DCP, LLDP, DCE/RPC connect, alarms, a GSDML file and certification. The open-source stack (p-net) has no Windows port. |
| PROFINET IRT / EtherCAT slave | Not realistic | These need hardware timing (an ASIC/FPGA). Keep the Hilscher card for them. |

**About timing:** Windows is not real-time. The library requests 1 ms timer resolution and runs its network thread at
`TIME_CRITICAL` priority. On a dedicated NIC with no heavy load, an RPI of **10 ms or more** is a safe choice. 4 ms can work.
Below 2 ms the library refuses the connection (`AdapterConfig::minRpiUs`). For hard real-time, use the Hilscher card.

## What's implemented (v1)

- Encapsulation: ListIdentity (TCP and UDP broadcast, so RSLinx and Studio 5000 can browse the device), ListServices,
  ListInterfaces, RegisterSession/UnRegisterSession, SendRRData
- CIP objects:
  - Identity (Get_Attributes_All, Get_Attribute_Single, Reset)
  - Assembly (Get attributes 3 and 4)
  - Connection Manager (Forward_Open, Large_Forward_Open, Forward_Close, Unconnected_Send)
- Class-1 cyclic I/O over UDP 2222, with three connection types:
  - exclusive owner
  - input-only (heartbeat 198)
  - listen-only (heartbeat 199)
- Validation, with standard extended status codes in the reply: electronic key, sizes, RPI, ownership conflicts,
  duplicate connections
- Run/idle header decoding. An inactivity watchdog drops the connection when the PLC stops sending, and the outputs then go to IDLE.

**Not yet:**
- multicast T→O (set the PLC connection to **Unicast**)
- class-3 connected messaging
- the TCP/IP and Ethernet Link objects
- an EDS file
- CIP Security

The roadmap below covers these.

## Assembly layout (defaults)

| Instance | Direction | Size | Notes |
|---|---|---|---|
| 100 | Input, T→O (PC → PLC) | 32 B | modeless |
| 150 | Output, O→T (PLC → PC) | 32 B | 32-bit run/idle header |
| 151 | Configuration | 0 B | |
| 198 / 199 | Input-only / listen-only heartbeat | 0 B | |

All of these are set in `softeip::AdapterConfig`.

## Build

Requires CMake 3.16 or later and a C++17 compiler.

**Windows: generate a Visual Studio 2026 solution (.slnx + .vcxproj):**
```bat
generate_vs2026.bat open
```
or, equivalently:
```bat
cmake --preset vs2026                         :: -> build\vs2026\SoftFieldbus.slnx
cmake --build --preset vs2026-release         :: optional command-line build
```
- The `Visual Studio 18 2026` generator needs **CMake 4.2 or newer**. The CMake bundled with VS 2026 is new enough;
  check with `cmake --version` in a Developer Command Prompt.
- For VS 2022, use `cmake --preset vs2022` instead.
- The solution shows the headers under *Header Files* and groups the projects into folders.
  `eip_adapter_demo` is the startup project, and `eip_scanner_sim` comes with debugger arguments for a local test.
- The `.vcxproj` files are generated, so don't edit or commit them. Change `CMakeLists.txt` and regenerate.
  Opening the folder directly in VS (File → Open → Folder) also works, with no solution needed.

**Linux (for development and CI):**
```sh
cmake --preset linux && cmake --build --preset linux -j
```

## Using the library

```cpp
#include "softeip/eip_adapter.hpp"

softeip::AdapterConfig cfg;
cfg.inputSize = 32;                  // bytes PC -> PLC
cfg.outputSize = 32;                 // bytes PLC -> PC
cfg.identity.productName = "My Vision Station";
cfg.onOutputs = [](const std::vector<uint8_t>& out, bool run) { /* network thread: keep it short */ };

softeip::Adapter adapter(cfg);
adapter.start();

// your application loop (MFC / WPF host / service ...)
std::vector<uint8_t> in(32);
adapter.setInputData(in.data(), in.size());  // thread safe
auto out = adapter.outputData();             // thread safe
bool plcRunning = adapter.plcInRun();
```

`Adapter` uses pimpl, so its header does not pull in `winsock2.h`. That lets you include it from MFC code without
the usual `windows.h` / `winsock.h` ordering problems. For a C# (WPF/WinUI/MAUI) front end, wrap it in a small C API
DLL or a C++/CLI shim.

## Using it from C# (WPF / WinUI / MAUI-Windows)

`SoftEip.Net.dll` is a C++/CLI mixed-mode assembly. From C# it is a normal .NET class, and the native
adapter is linked inside it. Visual Studio builds it as part of the generated solution, together with a C# sample
(`SoftEipSample`, which works like `eip_adapter_demo`).

```csharp
using SoftFieldbus;

var cfg = new EipAdapterConfig { InputSize = 32, OutputSize = 32, BindAddress = "192.168.1.50" };
using var eip = new EipAdapter(cfg);
eip.OutputsChanged += (s, e) =>            // raised on the network thread
    Dispatcher.BeginInvoke(() => Show(e.Data, e.PlcRun));
eip.ConnectionChanged += (s, e) => { /* e.Connected */ };
eip.Start();

eip.SetInputs(myInputs);                    // byte[], thread safe
byte[] outputs = eip.GetOutputs();
```

- **Target framework:** pick it with `-DSOFTEIP_DOTNET_FRAMEWORK=`. The default is `net8.0`. You can use
  `net10.0`, or `v4.8` for .NET Framework WPF/MFC hosts.
- **Deploy** `SoftEip.Net.dll`. For .NET 5+, also deploy `Ijwhost.dll`. Both are in `build/vs2026/bin/<Config>/`.
  The app must be **x64**.
- **Event handlers:** they run on the adapter thread. Keep them short, marshal to the UI thread, and never call
  `Stop()` or `Dispose()` from inside one.
- **MAUI:** this only works for the Windows target, because C++/CLI is Windows-only.

**Native C++ DLL instead of a static lib:** configure with `-DSOFTEIP_SHARED=ON` to get `softeip.dll`, which exports
`softeip::Adapter`. The consuming app must use the same compiler and runtime (/MD).

## Testing without a PLC

`eip_scanner_sim` acts as a PLC. It runs ListIdentity, a session, Forward_Open, cyclic I/O with an echo check, and Forward_Close:

```sh
eip_adapter_demo                       # terminal 1
eip_scanner_sim --target 127.0.0.1 --rpi-ms 10 --seconds 5 --local-port 2223   # terminal 2
```
When both run on the same PC, the simulator receives on `--local-port`, because the adapter already owns UDP 2222.
It tells the adapter which port to use through a Sockaddr Info item, the same way a real scanner does.
Other options (`--in-size`, `--out-size`, `--o2t 198`, `--skip-close 1`, ...) exercise the error paths.
`docs/phases/phase-3-demo-and-sim.md` lists them with their results.

### Test results (loopback, RPI 10 ms, 5 s)

| Test | Expected | Linux (g++) | Windows 11 (VS 2026) |
|---|---|---|---|
| Build, `/W4` / `-Wall -Wextra` | 0 warnings | ✅ 0 | ✅ 0 (Release + Debug, whole solution incl. C++/CLI + C#) |
| Happy path, `eip_adapter_demo` | ~500 T→O, echo, Forward_Close OK | ✅ 496–500/500 | ✅ 499–500/500 |
| Happy path, `SoftEipSample` (C#) | PASS | (Windows only) | ✅ 500/500 ×3 |
| O→T size 30 (adapter 32) | 0x0127 | ✅ | ✅ |
| T→O size 16 (adapter 32) | 0x0128 | ✅ | ✅ |
| RPI 1 ms (min 2 ms) | 0x0111 | ✅ | ✅ |
| O→T instance 155 | 0x0117 | ✅ | ✅ |
| Second exclusive owner | 0x0106 | ✅ | ✅ (first owner unaffected) |
| Input-only (198) | accepted | ✅ | ✅ 199/200 T→O |
| `--skip-close` | watchdog closes after 8×RPI | ✅ ~80 ms | ✅ "connection timed out" |

Echo matches are typically 450–490 out of 500. The simulator changes its outputs every 100 ms, and the echo
can lag one cycle at each change; that's expected, not a loss. Details and the fixes made for Windows:
`docs/phases/phase-3-demo-and-sim.md` (Linux) and `docs/phases/phase-5-dotnet-wrapper.md` → "Windows results".
Not yet measured: per-packet RPI jitter and a real PLC (next roadmap item).

## One API for every fieldbus

`softfb::FieldbusDevice` (C++) and `SoftFieldbus.FieldbusDevice` (.NET) work like a Hilscher cifX channel.
The application only sees two fixed byte areas, named from the **PC's point of view**:

| Area | Direction | API | cifX equivalent |
|---|---|---|---|
| **Input area** | PLC → PC | `ioRead(offset, data, len)` | `xChannelIORead` |
| **Output area** | PC → PLC | `ioWrite(offset, data, len)` | `xChannelIOWrite` |

The PC and the PLC agree on a layout (for example a shared struct) and cast the bytes. Changing fieldbus is
a configuration change; the application code stays the same:

```cpp
#include "softfb/fieldbus_device.hpp"

softfb::DeviceConfig cfg;
cfg.transport = softfb::Transport::ModbusTcp;   // EtherNetIP, ModbusTcp, ModbusUdp, ModbusTcpUdp
cfg.inputSize = 64;                             // bytes PLC -> PC
cfg.outputSize = 64;                            // bytes PC -> PLC
cfg.onStateChanged = [](softfb::DeviceState s) { /* WaitingForMaster, ConnectedRun, ... */ };

softfb::FieldbusDevice dev(cfg);
dev.start();
dev.ioRead(0, &fromPlc, sizeof fromPlc);
dev.ioWrite(0, &toPlc, sizeof toPlc);
```

```csharp
var cfg = new FieldbusDeviceConfig { Transport = FieldbusTransport.EtherNetIP, InputSize = 64, OutputSize = 64 };
using var dev = new FieldbusDevice(cfg);      // SoftFieldbus.Net.dll
dev.Start();
byte[] fromPlc = dev.IoRead();
dev.IoWrite(0, toPlc);
```

Where the areas live on each bus:

| Transport | Input area (PLC → PC) | Output area (PC → PLC) |
|---|---|---|
| EtherNet/IP | O→T output assembly 150 (with 32-bit run/idle header) | T→O input assembly 100 |
| Modbus | holding registers (FC03/06/16/22/23) and coils (FC01/05/15) | input registers (FC04) and discrete inputs (FC02) |

`fb_device_demo --transport eip|modbus-tcp|modbus-udp|modbus` and the C# `SoftFieldbusSample` run the same echo
application on every transport.

**About endianness:** Modbus registers are big-endian on the wire, and the area keeps that byte order (register k = bytes
2k high, 2k+1 low). EtherNet/IP data is copied as is. Agree on the byte order of multi-byte values with the PLC
programmer. For example, a float written by a Siemens PLC over Modbus arrives big-endian.

## Modbus TCP / UDP slave

`softmb::ModbusSlave` is a Modbus server on **TCP and/or UDP port 502**, with MBAP framing on both.

| Modbus table | Function codes | Area | Master access |
|---|---|---|---|
| Holding registers 0 … inputSize/2-1 | 03, 06, 16, 22, 23 | input area (PLC → PC) | read and write |
| Coils 0 … inputSize·8-1 (bit i = byte i/8, bit i%8) | 01, 05, 15 | input area, bit view | read and write |
| Input registers 0 … outputSize/2-1 | 04 | output area (PC → PLC) | read |
| Discrete inputs (same bit mapping) | 02 | output area, bit view | read |
| Optional `outputsInHoldingAt = k` | 03 | output area, read-only from holding register k | read |

- FC 43/14 (device identification) returns VendorName, ProductCode and Revision.
- Exceptions follow the spec: 01 illegal function, 02 illegal address, 03 illegal value.
- **Unit id:** `unitId = 0` answers every unit id. Any other value answers only that id, plus 0xFF and 0 (the values masters use to address a TCP device directly). Other ids get no reply.
- **Addressing:** many masters and HMIs number from 1 ("40001" = holding register **0**, "30001" = input register 0).
- **Masters that only use FC03/FC16** (some PLC function blocks): set `outputsInHoldingAt`, for example to 1000, to make the output area readable as holding registers.
- **Firewall:** allow inbound **TCP 502 and UDP 502**. Windows needs no admin rights for port 502 (Linux does; the tests use 1502).
- **RTU over serial** is not implemented (no COM ports on modern PCs). The PDU engine (`processPdu`) is transport independent, so RTU over a USB-RS485 adapter or RTU-over-TCP can be added.

**Testing without a PLC:**
```sh
mb_slave_demo --port 1502            # unit 1, 64 B input / 64 B output, echo + heartbeat
mb_master_sim --port 1502 --transport tcp    # or udp: 24 checks, RESULT: PASS
```
`mb_master_sim` covers every function code, the exception cases, the unit filter, TCP pipelining and the polling
rate (about 26 000 requests/s on loopback). Interop with [pymodbus](https://github.com/pymodbus-dev/pymodbus) 3.15 was
also checked; results are in `docs/phases/phase-6-modbus.md`.

## Connecting a real PLC

### Rockwell Studio 5000 (Generic Ethernet Module, no EDS needed)
1. I/O Configuration → Ethernet → New Module → **ETHERNET-MODULE** (Generic Ethernet Module).
2. Comm Format: **Data - SINT**. IP address: the PC's IP.
3. Assembly instances and sizes:
   - Input: **100**, size **32**
   - Output: **150**, size **32**
   - Configuration: **151**, size **0**
4. Connection tab:
   - RPI: **10 ms or more**
   - Tick **Use Unicast Connection over EtherNet/IP**
5. Download, then switch the PLC to RUN. The demo prints the outputs, and the PLC reads them back with bytes 0‑3 used as a heartbeat counter.

### Other scanners (Omron, Keyence, CODESYS, Beckhoff, ...)
Most of these need an **EDS file** to import the device. That is the next roadmap item. Until then, use a "generic
EtherNet/IP device" entry if the tool has one, with the same instances, sizes and a unicast connection.

### Windows checklist
- Firewall: allow inbound **TCP 44818, UDP 44818 and UDP 2222** for the executable.
- Timer resolution: Windows 11 ignores `timeBeginPeriod(1)` for processes without a visible window (a minimized HMI,
  a service). The library opts out of that power throttling itself, so the 1 ms resolution holds; without the
  opt-out, RPI 10 ms delivered only ~85 % of the packets. Apps that create their own timing loops should use a
  high-resolution waitable timer, as `eip_adapter_demo` does.
- Use a dedicated NIC for the machine network. Disable its power saving and interrupt moderation.
- Keep the PC's IP static.
- The vendor ID default is `0xFFFF` (a placeholder). Use your company's ODVA vendor ID for any product.

## Roadmap

1. **EDS file** so non-Rockwell PLCs can import the device.
2. **Multicast T→O.** Some PLCs default to it, and listen-only connections only really work with it.
3. **TCP/IP (0xF5) and Ethernet Link (0xF6) objects.** The conformance test requires them, and some scanners read them.
4. **Class-3 explicit messaging** (MSG instructions to read/write assemblies or parameters).
5. **Single bit and named variable access** on top of `ioRead`/`ioWrite`: `ioReadBit`/`ioWriteBit`, plus a
   variable map (name, type, offset, bit, byte order) shared with the PLC project, with typed get/set by name.
6. **Modbus RTU** over USB-RS485 adapters / RTU-over-TCP (reuses the Modbus PDU engine).
7. **PROFINET RT device** as a separate module: Npcap for Layer 2, then DCP, LLDP, RPC connect, cyclic RT and alarms, plus a GSDML file.
   This is a large effort, about 5–10× the work of EtherNet/IP. An alternative is porting p-net (GPL or commercial license) to Npcap.

**Build vs. reuse:** [OpENer](https://github.com/EIPStackGroup/OpENer) is a mature open-source EtherNet/IP adapter in C
with a Windows port. This repo is a small, readable, modern C++ implementation that is easy to embed and extend.
If you plan formal ODVA conformance, compare effort against OpENer or a commercial stack before going further.

## Repository layout

```
include/softfb/    FieldbusDevice: one Hilscher-style API for every transport
include/softeip/   EtherNet/IP adapter API + shared socket/byte helpers
include/softmb/    Modbus TCP/UDP slave API
src/               implementations
dotnet/            C++/CLI wrappers (SoftFieldbus.Net.dll, SoftEip.Net.dll) + C# samples
examples/          demo devices (fb_device_demo, eip_adapter_demo, mb_slave_demo)
tools/             PLC/master simulators (eip_scanner_sim, mb_master_sim)
docs/              plan, phase documents, HANDOFF.md (Windows build/verify steps)
```
