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
