# Plan: software-only EtherNet/IP adapter for Windows (C++)

## Context
Right now the team uses Hilscher hardware (netX/cifX) to put Windows PCs on PROFINET / EtherNet/IP, with the PLC as master. For slow I/O they want a software-only alternative on a standard NIC, written in C++. The repo `/home/user/FieldsBus` is empty (branch `claude/software-fieldbus-solution-yp9gxc`, no commits yet).

**How hard is it?**
- **EtherNet/IP adapter (device side):** easy to moderate. It only needs standard TCP and UDP sockets: TCP/UDP 44818 for explicit messaging and discovery, and UDP 2222 for cyclic class‑1 I/O. There is no raw Ethernet, no driver and no admin rights. It is a good first target.
- **PROFINET RT device:** much harder. It needs raw Layer‑2 frames (EtherType 0x8892, through Npcap on Windows), DCP, LLDP, DCE/RPC connect, alarms, and a GSDML file. The open-source p‑net stack has no Windows port. This is phase 2.
- **Timing:** Windows is not real-time. With 1 ms timer resolution and a high-priority thread, an RPI of 10 ms or more is realistic. Avoid anything under about 4 ms.

Outcome of this step: a working EtherNet/IP adapter library plus a demo, and a scanner (PLC) simulator so it can be tested without a real PLC.

## Already done (before planning)
- `include/softeip/bytes.hpp`: `ByteWriter` / `ByteReader` (little-endian for CIP, big-endian for sockaddr), plus `ParseError`.
- `include/softeip/socket_compat.hpp`: Winsock/POSIX wrapper. `SocketLibrary` handles WSAStartup and `timeBeginPeriod(1)`. Also `sendAll`, `sendTo`, `recvFrom`, `setRecvTimeoutMs`, and `disableUdpConnReset` (works around the Windows SIO_UDP_CONNRESET problem).

## Files to add
| File | Purpose |
|---|---|
| `include/softeip/cip_defs.hpp` | Encapsulation commands and status codes, CPF item types, CIP services, classes, general and extended status codes |
| `include/softeip/eip_adapter.hpp` | Public API with a pimpl, so no winsock in the header: `IdentityInfo`, `AdapterConfig` (assembly instances 100/150/151/198/199, sizes, minRpiUs, callbacks `onLog`, `onOutputs(data, run)`, `onConnectionChanged`), and class `Adapter` with `start` / `stop` / `setInputData` / `outputData` / `plcInRun` / `outputConnected` |
| `src/eip_adapter.cpp` | Protocol engine on a single network thread: a select() loop, with the thread set to TIME_CRITICAL priority on Windows |
| `examples/eip_adapter_demo.cpp` | Demo device: inputs echo the outputs, with bytes 0‑3 used as a heartbeat counter; handles Ctrl+C |
| `tools/eip_scanner_sim.cpp` | PLC simulator: ListIdentity, RegisterSession, Get_Attribute_Single on the product name, Forward_Open, cyclic O→T and T→O I/O, echo check, Forward_Close. Exits with 0 or 1 |
| `CMakeLists.txt` | C++17, `softeip` static lib, links `ws2_32` and `winmm` on Windows plus Threads, two executables, /W4 or -Wall -Wextra |
| `README.md` | Difficulty assessment, architecture, build steps (VS 2022 / CMake), Studio 5000 "Generic Ethernet Module" setup, firewall ports, Windows timing notes, roadmap (EDS file, multicast T→O, TCP/IP and Ethernet Link objects, PROFINET via Npcap, a comparison with OpENer) |
| `.gitignore` | `build/`, `out/`, `.vs/` |

## What the adapter implements
- **Encapsulation (TCP 44818):** NOP, ListIdentity, ListServices, ListInterfaces, RegisterSession, UnRegisterSession and SendRRData. Session handles are validated, and replies use the correct encapsulation status codes.
- **UDP 44818:** answers broadcast ListIdentity. The local IP is found with a connect()+getsockname() trick.
- **CIP objects:**
  - Identity (0x01): Get_Attributes_All, Get_Attribute_Single for attributes 1‑7, and Reset. The status word reflects the owned bit and the I/O state.
  - Assembly (0x04): Get attribute 3 (data) and attribute 4 (size).
  - Connection Manager (0x06): Forward_Open, Large_Forward_Open, Forward_Close and Unconnected_Send.
- **Forward_Open checks:**
  - Parses the electronic key (0x34) and checks vendor, product, device type and revision, including the compatibility bit.
  - Parses connection points written as 0x24/0x2C (8 or 16 bit), port segments and data segments.
  - Supports three connection types: exclusive owner (150→100), input‑only (198) and listen‑only (199).
  - Checks the class‑1 transport and the unicast P2P connection type.
  - Requires exact sizes: O→T is the output size + 6 (sequence count + run/idle header); T→O is the input size + 2.
  - Enforces the minimum RPI, allows at most 8 connections, and detects duplicate or ownership conflicts.
  - Each failure returns its CM extended status: 0x0100, 0x0103, 0x0106, 0x0109, 0x0111, 0x0113, 0x0114‑0x0116, 0x0117, 0x0119, 0x0123, 0x0124, 0x0127, 0x0128, or 0x0315.
  - The adapter picks the O→T connection ID; the T→O ID comes from the originator. A T→O port given in a 0x8001 sockaddr item is honoured.
- **Class‑1 I/O (UDP 2222):**
  - Parses CPF packets with item types 0x8002 and 0x00B1, and checks the source IP.
  - Rejects duplicates using the 16‑bit sequence number.
  - Decodes the run/idle header and copies the outputs, firing a callback only when they change.
  - Produces T→O packets every RPI.
  - Inactivity watchdog: 4<<multiplier × RPI, with a 10 s grace period before the first packet. When the owner connection is lost, `run=false` and the listen‑only connections are closed.
- **Not in v1:** multicast T→O (the PLC must be set to Unicast), class‑3 connected messaging, the TCP/IP and Ethernet Link objects, the EDS file, and CIP Security. All of these are listed in the README as next steps.

## Phases (one commit each, build must pass at every phase)
0. **Docs in the repo:**
   - `docs/PLAN.md`: this plan (context, difficulty assessment, scope, verification).
   - `docs/phases/phase-1-foundation.md` through `phase-4-docs.md`: one file per phase, each with its goal, files, tasks as a checklist, and done criteria.
   - Each phase commit ticks off its checklist.
1. **Foundation and discovery:**
   - Write `cip_defs.hpp`, `eip_adapter.hpp` and `.gitignore`, plus the already-done `bytes.hpp` and `socket_compat.hpp`.
   - Write `CMakeLists.txt`.
   - In `eip_adapter.cpp`, write the socket setup, the select loop and the encapsulation layer: ListIdentity/ListServices/ListInterfaces on TCP and UDP, RegisterSession/UnRegisterSession, and SendRRData with a CIP dispatcher for Identity and Assembly Get.
   - Check: build passes, and a quick UDP ListIdentity probe works.
2. **Connections and I/O:**
   - Connection Manager: Forward_Open, Large_Forward_Open, Forward_Close and Unconnected_Send, with all the checks and extended status codes.
   - Class‑1 consume/produce on UDP 2222, the watchdog, run/idle handling and the callbacks.
3. **Demo and simulator:**
   - Write `examples/eip_adapter_demo.cpp` and `tools/eip_scanner_sim.cpp`.
   - Run the end-to-end and negative tests from Verification below, and fix any bugs found.
4. **Docs:**
   - Write `README.md` (difficulty, Studio 5000 setup, Windows timing, firewall, roadmap including PROFINET).
   - Push to the branch.

## Verification
1. `cmake -S . -B build && cmake --build build -j` builds with no warnings on Linux (g++). The code is also written to build with MSVC.
2. Run `./build/eip_adapter_demo &` and then `./build/eip_scanner_sim --target 127.0.0.1 --rpi 10 --seconds 5 --local-port 2223`. Expect the identity name to be printed, Forward_Open OK, about 500 T→O packets, the echo check passing, Forward_Close OK, and exit code 0.
3. Negative tests: a wrong size gives 0x0127/0x0128, an RPI that is too fast gives 0x0111, and a second owner gives 0x0106. Running the simulator with `--skip-close` shows the adapter log "connection timed out" after the watchdog expires.
4. Commit and push to `claude/software-fieldbus-solution-yp9gxc` (`git push -u origin ...`). No PR.

## Follow-ups (after the initial 4 phases)
- **VS 2026 solution generation:** `CMakePresets.json` + `generate_vs2026.bat`
- **Phase 5, .NET wrapper:** C++/CLI `SoftEip.Net.dll` + C# sample + optional native DLL.
  See `docs/phases/phase-5-dotnet-wrapper.md`.
- **Phase 6:** Modbus TCP/UDP slave (`softmb`) and the transport-independent, Hilscher-style
  `FieldbusDevice` API (C++ `softfieldbus`, .NET `SoftFieldbus.Net`). Process image in PC view:
  input area = PLC → PC (`ioRead`), output area = PC → PLC (`ioWrite`).
  See `docs/phases/phase-6-modbus.md`. Deferred: single-bit/named-variable access, Modbus RTU.

---

## Phase 6 plan: Modbus slave (TCP + UDP) and Hilscher-style FieldbusDevice API

### Context
Besides EtherNet/IP, the team needs a software **Modbus slave (server)** on Windows. A PLC or SCADA master polls it over **Modbus TCP and Modbus UDP**.
- **RTU (serial)** is deferred, because modern PCs no longer have COM ports. The design keeps the PDU layer independent of the transport, so RTU or RTU-over-TCP can be added later.
- **Location:** the user chose the same repo (FieldsBus) as a new library `softmb` next to `softeip`.

**Difficulty:** low. Modbus is request/response and has no cyclic I/O or connection management. It's roughly ⅓ of the EtherNet/IP work.

### Reuse
- `include/softeip/socket_compat.hpp`: Winsock/POSIX, `SocketLibrary` (incl. Win11 timer fix), `sendAll`, `sendTo`/`recvFrom`, `disableUdpConnReset`.
- `include/softeip/bytes.hpp`: `ByteWriter::u16be`, `ByteReader::u16be` (Modbus is big-endian).
- **Structure:** follow the patterns of `src/eip_adapter.cpp` (pimpl, one `select()` network thread, the TCP client reassembly loop, thread-safe process image with a mutex, callbacks) and of `dotnet/` (C++/CLI wrapper, CMake `/clr` handling, C# sample via `include_external_msproject`).
- **CMake:** in `CMakeLists.txt`, use the `softeip_warnings()` function and the common `bin/` output folder.
- **Shared headers:** keep them where they are to avoid churn in the Windows-verified code, and expose them through a small CMake INTERFACE target `softfieldbus_common` that both libs link.

### Guiding principle: a cifX-like process image, **PC view** (user requirement)
Every transport exposes the same two **fixed byte areas**, named from the PC's point of view as Hilscher does:
- **Input area** (PLC → PC): the master writes it, the app reads it (`ioRead`).
- **Output area** (PC → PLC): the app writes it, the master reads it (`ioWrite`).

The PC and the PLC each cast these bytes to agreed data types (a shared struct or layout document). A thin layer selects the transport, so the app code stays the same.

**Naming note:** `softeip` keeps the CIP device-view names it already has, and these are the opposite way round:
- its input assembly 100 (T→O) is the PC **output** area
- its output assembly 150 (O→T) is the PC **input** area

The new layer hides this. `softmb` and the layer both use the PC view.

### Library `softmb`
**Modbus mapping onto the two areas.** Wire byte order equals area byte order: register k is bytes [2k] (high) and [2k+1] (low).

| Modbus table (master's view) | Function codes | PC area (Hilscher PC view) | Master access |
|---|---|---|---|
| Holding registers 0..m | FC03/06/16/22/23 | **input area** (PLC → PC) | read and write |
| Coils (bit i = byte i/8, bit i%8) | FC01/05/15 | **input area** (same bytes, bit view) | read and write |
| Input registers 0..n | FC04 | **output area** (PC → PLC) | read |
| Discrete inputs (same bit mapping) | FC02 | **output area** (bit view) | read |
| Optional `outputsInHoldingAt = k` | FC03 | output area mirrored read-only at holding k… | for masters that only speak FC03/FC16; writes there → exception 02 |

**Public header** `include/softmb/modbus_slave.hpp`, no winsock (pimpl):
- **`ModbusSlaveConfig`:**
  - `bindAddress`, `port = 502`, `enableTcp = true`, `enableUdp = true`
  - `unitId` (0 = answer any unit)
  - `inputSize` (PLC → PC) and `outputSize` (PC → PLC), in bytes (even; default 64 each)
  - `outputsInHoldingAt` (optional)
  - `maxTcpClients = 8`, `tcpIdleTimeoutMs`
  - identity strings for FC 43/14
  - callbacks `onLog` and `onInputsChanged(const std::vector<uint8_t>&)` (the master wrote), plus `onConnectionChanged`
- **`ModbusSlave`:** PC-view API
  - `start` / `stop`
  - `ioRead(offset, data, len)`: reads the input area (PLC → PC)
  - `ioWrite(offset, data, len)`: writes the output area (PC → PLC)
  - `inputData()`: snapshot of the input area
  - `masterConnected()`: a TCP client is connected, or a request arrived within the watchdog time

**`src/modbus_slave.cpp`:**
- **PDU core:** `processPdu(const uint8_t*, size_t) -> response PDU`. It's transport-independent and reusable for RTU later.
  - **Function codes:** 01, 02, 03, 04, 05, 06, 15, 16, 22 (mask write), 23 (read/write multiple) and 43/14 (device identification, basic objects).
  - **Limits:** 2000 bits / 125 registers read, 1968 bits / 123 registers write.
  - **Exceptions:** 01 illegal function, 02 illegal address, 03 illegal value.
- **TCP on port 502:** MBAP header (transaction, protocol = 0, length, unit). Stream reassembly per client; several requests in one segment are handled; the protocol id and length are validated; misbehaving clients are dropped; idle timeout.
- **UDP on port 502:** same MBAP framing, one request per datagram, reply to the sender.
- **Unit-id filter:** a non-matching unit gets no reply, like a gateway with no target.

### Tools, demo, .NET
- **`examples/mb_slave_demo.cpp`:** copies the input area (what the PLC wrote) into the output area, with bytes 0..3 used as a heartbeat (the same idea as the EtherNet/IP demo). It logs input changes. Options: `--port`, `--no-udp`, `--unit`, `--in-size`, `--out-size`.
- **`tools/mb_master_sim.cpp`:** a test master over `--transport tcp|udp`.
  - Exercises every function code and checks the values against what was written.
  - Exception cases: bad address → 02, bad quantity → 03, unknown function code → 01.
  - Wrong unit id → timeout.
  - Several requests pipelined in one TCP segment.
  - Exits 0 or 1 with `RESULT: PASS/FAIL`.
- **CMake:** add `softmb`, `mb_slave_demo` and `mb_master_sim`.

### Thin transport-selection layer (prepared now, tiny)
Both protocols share the same process-image API, so the layer is small and can be added now or later without touching the protocol code:
- **`include/softfb/fieldbus_device.hpp`:**
  - `enum class Transport { EtherNetIP, ModbusTcp, ModbusUdp, ModbusTcpUdp }`
  - `struct DeviceConfig { Transport; bindAddress; inputSize; outputSize; protocol-specific sub-structs }`
  - `class FieldbusDevice` (pimpl, created from `DeviceConfig`), shaped like cifX:
    - `start` / `stop`
    - `ioWrite(offset, data, len)`: PC → PLC, like `xChannelIOWrite`
    - `ioRead(offset, data, len)`: PLC → PC, like `xChannelIORead`
    - `state()`: Stopped / WaitingForMaster / Connected (Run/Idle)
    - `onInputsChanged` (the PLC wrote new data)
- **`src/fieldbus_device.cpp`:** dispatches to `softeip::Adapter` or `softmb::ModbusSlave`. Library `softfieldbus` links both.
- **.NET:** one new C++/CLI assembly `SoftFieldbus.Net.dll` with a `FieldbusDevice` class and a `Transport` enum (`byte[]` `IoRead`/`IoWrite`, events), built with the existing `/clr` CMake settings (factored into a function). `SoftEip.Net` stays unchanged, because it's already Windows-verified. There is no separate Modbus .NET wrapper; C# uses the generic one.
- **`examples/fb_device_demo.cpp`:** `--transport eip|modbus-tcp|modbus-udp|modbus` runs the same echo app on any transport. This proves the abstraction with both existing simulators.

### Later (recorded, not in this phase)
- **Single coil / single register access** on top of the byte areas, e.g. `ioReadBit(bitOffset)` / `ioWriteBit`.
- **Variable accessibility:** a variable map (name, data type, offset, bit), loaded from a JSON/CSV file shared by the PC and PLC projects, with typed get/set by name (`dev.get<float>("Speed")`) and optional endianness/word-swap per variable. It sits on top of `ioRead`/`ioWrite`, so it works for every transport.
- Modbus RTU (USB-RS485) / RTU-over-TCP.

### Docs
- `docs/phases/phase-6-modbus.md`: checklist and results table.
- README: a Modbus section with a register-map table, master setup (port 502, unit id, 0- vs 1-based addressing note) and firewall (TCP+UDP 502).
- `docs/HANDOFF.md`: Windows verification steps for the Modbus parts.
- Roadmap: RTU over USB-RS485 adapters / RTU-over-TCP.

### Verification (Linux here; Windows via HANDOFF)
1. `cmake --preset linux && cmake --build --preset linux` builds with 0 warnings. The EtherNet/IP tests still pass.
2. Run `mb_slave_demo --port 1502` (Linux needs root for <1024; Windows doesn't). Then `mb_master_sim --port 1502 --transport tcp` and `--transport udp` must both PASS, including all the exception cases.
3. Independent interop check, if pip works through the proxy: a `pymodbus` client script reads and writes the registers over TCP and UDP. It's a test aid only and isn't shipped.
4. Run `fb_device_demo --transport eip` with `eip_scanner_sim`, then `--transport modbus` with `mb_master_sim` (tcp and udp). Both must PASS with the same app code.
5. Commits, one per step, pushed to `claude/software-fieldbus-solution-yp9gxc`:
   - 6a: `softmb` + demo + master simulator + tests
   - 6b: the `FieldbusDevice` layer + `fb_device_demo`
   - 6c: `SoftFieldbus.Net` + C# sample
   - 6d: docs and HANDOFF

   The C++/CLI part can't be verified here; it gets flagged in HANDOFF for the Windows session.

> Status: all four steps (6a–6d) are done; the results are in `docs/phases/phase-6-modbus.md`.
