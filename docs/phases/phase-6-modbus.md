# Phase 6: Modbus slave (TCP + UDP) and transport-selection layer

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
- [ ] Wrapper + sample (cannot be compiled here; Windows verification goes through HANDOFF)

### 6d: Docs
- [ ] README Modbus section, HANDOFF, roadmap

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

## Later (recorded, not in this phase)
- Single coil / single register access helpers (`ioReadBit` / `ioWriteBit`).
- Variable map: name, type, offset, bit, from a JSON/CSV shared with the PLC project, with typed get/set and per-variable endianness.
- Modbus RTU (USB-RS485) / RTU-over-TCP. These reuse `processPdu`.
