# FieldsBus: software fieldbus device for Windows (C++)

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

**Windows (Visual Studio 2022):**
```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\eip_adapter_demo.exe
```
You can also open the folder in Visual Studio (File → Open → Folder), which uses the CMake support directly.

**Linux (for development and CI):**
```sh
cmake -S . -B build && cmake --build build -j
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
- Use a dedicated NIC for the machine network. Disable its power saving and interrupt moderation.
- Keep the PC's IP static.
- The vendor ID default is `0xFFFF` (a placeholder). Use your company's ODVA vendor ID for any product.

## Roadmap

1. **EDS file** so non-Rockwell PLCs can import the device.
2. **Multicast T→O.** Some PLCs default to it, and listen-only connections only really work with it.
3. **TCP/IP (0xF5) and Ethernet Link (0xF6) objects.** The conformance test requires them, and some scanners read them.
4. **Class-3 explicit messaging** (MSG instructions to read/write assemblies or parameters).
5. **A C API / C# wrapper** for WPF/WinUI/MAUI HMIs.
6. **PROFINET RT device** as a separate module: Npcap for Layer 2, then DCP, LLDP, RPC connect, cyclic RT and alarms, plus a GSDML file.
   This is a large effort, about 5–10× the work of EtherNet/IP. An alternative is porting p-net (GPL or commercial license) to Npcap.

**Build vs. reuse:** [OpENer](https://github.com/EIPStackGroup/OpENer) is a mature open-source EtherNet/IP adapter in C
with a Windows port. This repo is a small, readable, modern C++ implementation that is easy to embed and extend.
If you plan formal ODVA conformance, compare effort against OpENer or a commercial stack before going further.

## Repository layout

```
include/softeip/   public API (eip_adapter.hpp) + protocol helpers
src/               adapter implementation
examples/          demo device
tools/             PLC/scanner simulator
docs/              plan and phase documents
```
