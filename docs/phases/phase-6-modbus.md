# Phase 6: Modbus slave (TCP + UDP) and transport-selection layer

> **Update (2026-10-05, phase 7):** renamed for role-explicit Modbus terms: `ModbusSlave` → `ModbusServer`, `mb_slave_demo` → `mb_server_demo`, `mb_master_sim` → `mb_client_test` (now built on `softmb::ModbusClient`). This document keeps the names used at the time. See README "Roles" and `docs/phases/phase-7-modbus-client.md`.

**Goal:** a software Modbus slave next to the EtherNet/IP adapter. Every transport exposes the same
**Hilscher-style process image (PC view)**:
- **input area** = PLC → PC, read by the app with `ioRead`
- **output area** = PC → PLC, written by the app with `ioWrite`

PC and PLC cast the bytes to agreed data types. A thin layer selects the transport.

## Modbus mapping
Register k = area bytes [2k] (high) and [2k+1] (low), so the area has the same byte order as the Modbus wire.

| Modbus table | Function codes | PC area | Master access |
|---|---|---|---|
| Holding registers | 03, 06, 16, 22, 23 | input area (PLC → PC) | read and write |
| Coils (bit i = byte i/8, bit i%8) | 01, 05, 15 | input area (bit view) | read and write |
| Input registers | 04 | output area (PC → PLC) | read |
| Discrete inputs | 02 | output area (bit view) | read |
| `outputsInHoldingAt = k` (optional) | 03 | output area mirrored read-only from holding register k | read |

FC 43/14 (device identification, basic objects) is also supported.

## Steps
### 6a: `softmb` library, demo, master simulator
- [x] `include/softmb/modbus_slave.hpp`, `src/modbus_slave.cpp`
  - transport-independent PDU engine (`processPdu`)
  - MBAP over TCP (stream reassembly, pipelining, idle timeout, client limit) and over UDP
  - unit-id filter
- [x] `examples/mb_slave_demo.cpp`: echoes the input area into the output area; bytes 0..3 are a heartbeat
- [x] `tools/mb_master_sim.cpp`: every function code, exceptions, unit filter, pipelining, request rate
- [x] Interop with a third-party master (pymodbus 3.15, test aid only, not shipped)

### 6b: `FieldbusDevice` layer + `fb_device_demo`
- [x] `include/softfb/fieldbus_device.hpp`, `src/fieldbus_device.cpp`
  - `Transport` enum, `ioRead`/`ioWrite`, `DeviceState`
  - maps the PC view onto the CIP assemblies (O→T 150 = input area, T→O 100 = output area)
- [x] `examples/fb_device_demo.cpp`: the same echo app on every transport

### 6c: `SoftFieldbus.Net` (C++/CLI) + C# sample
- [x] `dotnet/SoftFieldbusNet.h/.cpp`: `SoftFieldbus.FieldbusDevice`, `FieldbusTransport` and `FieldbusState` enums, `byte[] IoRead()`, `IoRead(offset, buffer)`, `IoWrite(offset, data)` (also before `Start`), events `InputsChanged` / `StateChanged` / `Log`
- [x] `dotnet/fieldbus_sample/`: C# sample, transport chosen on the command line
- [x] `dotnet/CMakeLists.txt`: `/clr` settings factored into `softfb_clr_assembly()`, shared with `SoftEip.Net`
- [x] **Build and run on Windows / VS 2026**: whole solution 0 warnings (Release + Debug); every test below PASS (see "Windows results")

### 6d: Docs
- [x] README: "One API for every fieldbus" and "Modbus TCP / UDP slave" sections, roadmap, repo layout
- [x] HANDOFF: phase-6 status rows, Windows steps, trouble spots

## Results 6a (Linux, loopback, port 1502)
| Test | TCP | UDP |
|---|---|---|
| `mb_master_sim`: 24 checks (FC 01/02/03/04/05/06/15/16/22/23/43, 9 exception cases, unit filter, app echo + heartbeat) | PASS | PASS |
| 3 requests pipelined in one TCP segment | PASS | n/a |
| Polling rate, FC03 × 32 registers | ~26 000 req/s, 0.038 ms RTT | ~26 000 req/s, 0.038 ms RTT |
| pymodbus 3.15: FC16/03, coil, float32 round trip through the app, exception 02 | PASS | PASS |
| EtherNet/IP regression (`eip_scanner_sim`) | PASS | |

## Results 6b: one application, every transport (`fb_device_demo`)
| Transport | Test master | Result | States reported |
|---|---|---|---|
| EtherNet/IP | `eip_scanner_sim --in-size 64 --out-size 64` | PASS (300/300 T→O, echo) | WaitingForMaster → ConnectedIdle → ConnectedRun → WaitingForMaster → Stopped |
| Modbus TCP | `mb_master_sim --transport tcp` | PASS (24 checks) | WaitingForMaster → ConnectedRun → Stopped |
| Modbus UDP | `mb_master_sim --transport udp` | PASS | WaitingForMaster → ConnectedRun → Stopped |
| Modbus TCP+UDP | both, one after the other | PASS / PASS | WaitingForMaster → ConnectedRun → Stopped |

## Windows results (2026-10-05)

**Machine and tools:** Windows 11 (10.0.26200), VS 2026 Enterprise (MSVC 19.51, v145), Windows SDK 10.0.26100,
CMake 4.4.0-rc2, .NET SDK 8.0.425. Commit under test: `3319f0e`.

**Setup:** loopback 127.0.0.1. Every server was started with `--bind 127.0.0.1` (the C# sample takes the bind IP as its second
argument), so there was no firewall prompt. Modbus used port 502, which needs no admin rights on Windows.

**Build**

| Target | Result |
|---|---|
| `generate_vs2026.bat` / `cmake --preset vs2026` | ✅ `SoftFieldbus.slnx` with 12 projects, including the new `softmb`, `softfieldbus`, `mb_slave_demo`, `mb_master_sim`, `fb_device_demo`, `SoftFieldbus.Net`, `SoftFieldbusSample` |
| Whole solution, `msbuild -restore`, Release and Debug | ✅ 0 errors, **0 warnings** (`/W4`). No code changes were needed; `3319f0e` had already removed the one C4018 |
| `SoftFieldbusSample` → `SoftFieldbus.Net.dll` load | ✅ `Private=true` is in the template, and `SoftFieldbus.Net` is listed in `SoftFieldbusSample.deps.json` |

**Native Modbus (`mb_slave_demo` + `mb_master_sim`, port 502)**

| Test | TCP | UDP |
|---|---|---|
| `mb_master_sim` checks (FC 01/02/03/04/05/06/15/16/22/23/43, exceptions, unit filter, echo + heartbeat) | ✅ PASS, 24/24 | ✅ PASS, 23/23 (pipelining is TCP-only) |
| 3 requests pipelined in one TCP segment | ✅ PASS | n/a |
| Polling rate, FC03 × 32 registers | ~22 400–23 700 req/s, 0.042–0.045 ms RTT | ~17 500–18 500 req/s, 0.054–0.057 ms RTT |

**One application, every transport.** The server is `fb_device_demo` (native) and `SoftFieldbusSample` (C#); the test
master is `eip_scanner_sim --in-size 64 --out-size 64 --local-port 2223` for EtherNet/IP and `mb_master_sim` for Modbus.

| Transport | Master | `fb_device_demo` | `SoftFieldbusSample` (C#) | States reported |
|---|---|---|---|---|
| EtherNet/IP (`eip`) | `eip_scanner_sim`, RPI 10 ms, 5 s | ✅ PASS, 499–500/500 T→O | ✅ PASS, 494–500/500 T→O | WaitingForMaster → ConnectedIdle → ConnectedRun → WaitingForMaster |
| Modbus TCP+UDP (`modbus`) | `mb_master_sim` tcp, then udp | ✅ PASS / PASS | ✅ PASS / PASS | WaitingForMaster → ConnectedRun → WaitingForMaster |
| Modbus TCP (`modbus-tcp`) | tcp | ✅ PASS | ✅ PASS | same |
| Modbus UDP (`modbus-udp`) | udp | ✅ PASS | ✅ PASS | same |
| Negative: `modbus-tcp` + UDP master | udp | ✅ no UDP listener (master reports 22 failures, as expected) | ✅ same | — |
| Negative: `modbus-udp` + TCP master | tcp | ✅ "cannot open TCP connection" (no TCP listener, as expected) | ✅ same | — |

The last state is `WaitingForMaster` because the test processes were ended with `taskkill /F`; a clean Ctrl+C
also reports `Stopped`, as in the Linux runs.

**Linux re-check (WSL Ubuntu 24.04, g++, port 1502):** 0 warnings. `mb_master_sim` tcp/udp PASS, `fb_device_demo`
modbus tcp/udp PASS, `fb_device_demo` eip PASS (498/500).

## Later (recorded, not in this phase)
- Single coil / single register access helpers (`ioReadBit` / `ioWriteBit`).
- Variable map: name, type, offset, bit, from a JSON/CSV shared with the PLC project, with typed get/set and per-variable endianness.
- Modbus RTU (USB-RS485) / RTU-over-TCP. These reuse `processPdu`.
