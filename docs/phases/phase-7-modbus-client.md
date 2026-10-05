# Phase 7: Modbus TCP / UDP client (the PC polls a PLC) and role-explicit names

**Goal:** the PC as Modbus **client** (formerly "master"). It polls a PLC, a Wago coupler or a gateway that is the
Modbus **server** (formerly "slave"). This is what Dev.3 `UvcIOModBus` (libmodbus) does today, here without
libmodbus and without its limits. In the same phase, every Modbus name says its role.

## Roles (also in README "Roles")

| The PC is the… | Old term | The PLC is the… | Holds the registers | Class | Tools |
|---|---|---|---|---|---|
| **Modbus server** | slave | client (polls the PC) | the PC | `softmb::ModbusServer` | `mb_server_demo` |
| **Modbus client** | master | server (polled by the PC) | the PLC | `softmb::ModbusClient`, `softmb::ModbusClientPoller` | `mb_client`, `mb_client_test` |

**Renamed:**
- `ModbusSlave` → `ModbusServer`, `ModbusSlaveConfig` → `ModbusServerConfig`
- `masterConnected()` → `clientConnected()`, `masterTimeoutMs` → `clientTimeoutMs`
- `mb_slave_demo` → `mb_server_demo`, `mb_master_sim` → `mb_client_test`

`softmb/modbus_slave.hpp` is a deprecated forwarding header that keeps the old names for one release. The
`FieldbusDevice` / .NET enum value `WaitingForMaster` keeps its name, for API compatibility, and is documented as
"waiting for the scanner / Modbus client".

## Parity with UvcIOModBus

| UvcIOModBus (Dev.3) | Phase 7 |
|---|---|
| Polling thread every `ReadPolling` ms → cached bit image | `ModbusClientPoller`: every `cycleMs` → input image |
| Max 32 input / 32 output bits | Any number of areas and any size (split into protocol-sized requests) |
| FC01/FC02 reads, FC15 writes of the whole image, FC03/FC16 calls | Image: any table for reads; coils or holding registers for writes. `ModbusClient`: FC01/02/03/04/05/06/15/16/22/23 + raw `transact()` |
| UDP default, TCP optional; port fixed at 502; unit id not configurable; no timeouts | TCP or UDP; port, unit id, connect/response timeout, retries and reconnect delay all configurable |
| Reconnect through `tryToReOpen()` on any error | TCP reconnect (delayed after a failure); UDP resend; outputs rewritten after an error |
| libmodbus 3.0.6 (LGPL, patched for UDP) | Own code (`src/modbus_client*.cpp`), no dependency |

## Steps
### 7a: rename + shared pieces
- [x] `git mv` of the server files, classes and tools; deprecated aliases; a role line at the top of every Modbus header and log
- [x] `include/softmb/modbus_defs.hpp`: function/exception codes, limits, MBAP sizes, bit packing, `exceptionName()`. The server uses it.
- [x] `include/softeip/periodic_timer.hpp`: `PeriodicTimer`, moved from `eip_adapter_demo.cpp`
- [x] `socket_compat.hpp`: `connectWithTimeout`, `setNonBlocking`, `setSendTimeoutMs`, `waitReadable`

### 7b: `softmb::ModbusClient`
- [x] Synchronous and thread-safe; `Result {code, exception}`; no C++ exceptions
- [x] TCP:
  - connect with a timeout;
  - a receive buffer that cuts whole frames;
  - stale replies skipped by transaction id;
  - reconnect with `reconnectDelayMs`;
  - connect failures are not retried within a request.
- [x] UDP: replies matched by source address and transaction id; resend on timeout
- [x] Local validation against the protocol limits (`InvalidArgument`, no traffic)
- [x] `tools/mb_client_test.cpp`, formerly `mb_master_sim`, built on the client:
  - the server's exception cases go through raw `transact()`;
  - new client checks.

### 7c: `softmb::ModbusClientPoller`
- [x] Reads into the input image, any table; reads are published atomically
- [x] Writes the output image to coils or holding registers, on change or every cycle, and again after an error
- [x] Areas split into several requests (bit chunks are multiples of 8); `PeriodicTimer`; online state; stats
- [x] Checks in `mb_client_test`: echo, coils, discrete inputs, heartbeat, config validation, and a split test against an in-process 600-byte `ModbusServer`

### 7d: `mb_client` CLI + docs
- [x] `tools/mb_client.cpp`:
  - `read-holding|read-input|read-coils|read-discrete`;
  - `write-register(s)`, `write-coil(s)`;
  - `poll --read/--write TABLE:ADDR:COUNT`.
- [x] README (Roles, client section, renames, roadmap, layout), PLAN phase 7, HANDOFF rows and trouble spots
- [ ] EasyModbus over **TCP**: its TCP listener was off during the test, so only UDP was tested (see below)
- [ ] Visual check in EasyModbus' UI of the written values and the +1 address offset; value changed in the UI shows up in `mb_client poll`

## Windows results (2026-10-05, VS 2026, MSVC 19.51, loopback)

**Build:** the whole `SoftFieldbus.slnx` (13 projects) builds in Release and Debug with **0 warnings** (`/W4`).

**`mb_client_test` against `mb_server_demo --bind 127.0.0.1` (port 502):**

| Area | TCP | UDP |
|---|---|---|
| All checks | ✅ PASS **34/34** (×2) | ✅ PASS **32/32** (×2) |
| Function codes through the client API (FC01/02/03/04/05/06/15/16/22/23) + FC43 raw | ✅ | ✅ |
| Server exceptions (raw requests the client would refuse) | ✅ 9 cases | ✅ 9 cases |
| Client-side validation (`InvalidArgument`, < 50 ms, no traffic) | ✅ | ✅ |
| Unit filter → `Timeout`, then the next request is answered | ✅ | ✅ |
| TCP pipelining (raw), reconnect after `close()` | ✅ | n/a |
| No server: TCP `NotConnected` after 500 ms (connect timeout) / UDP `Timeout` after 2 × 200 ms | ✅ | ✅ |
| Poller, 10 ms cycle: echo through registers + discrete inputs, coils, heartbeat, stats | ✅ cycle 0.56–1.07 ms, max period 11.4–12.5 ms | ✅ cycle 0.35–0.41 ms, max period 10.6–10.8 ms |
| Poller split: 300 registers (3 writes + 3 reads) + 4800 coils (3 reads) | ✅ | ✅ |
| Request rate, FC03 × 32 registers | ~17 000 req/s, 0.06 ms round trip | ~15 000–17 000 req/s |

The client's TCP rate is below the old raw tool's (~23 000 req/s) because each request takes a lock and waits in
`select` with a deadline. The first version did two `recv` calls per reply and reached only ~10 600 req/s; the
receive buffer brought it up.

**Reconnect (`mb_client poll`, 100 ms cycle, server killed after ~8 s and restarted after ~12 s):**
- **TCP:** "send failed" → offline → a connect attempt every ~1 s → reconnected ~1 s after the restart → online, with values flowing again (180 updates).
- **UDP:** "no response" → offline → "answering" → online.

**Interop with EasyModbus Server Simulator (UDP 502):**

| Test | Result |
|---|---|
| `read-holding`, `read-input`, `read-coils`, `read-discrete` | ✅ |
| `write-register 9 4321`, `write-registers 10 100 200 300` → `read-holding 9 4` | ✅ 4321 / 100 / 200 / 300 |
| `write-coil 7 1`, `write-coils 20 1 0 1 1` → `read-coils 0 24` | ✅ |
| `read-holding 65530 6` (past the end of its table) | ✅ exception 02 (illegal data address), shown by name |
| `read-holding 65530 10` | ✅ rejected locally (`InvalidArgument`: address + count > 65536) |
| `poll --write holding:100:4 --read holding:100:4 --read coils:0:24`, 100 ms, 5 s | ✅ 50 cycles, 0 failed, counter 1..5 read back, max period 100.6 ms |

**Finding:** during the first test runs, both `mb_server_demo` and EasyModbus had **UDP port 502** open, with
`SO_REUSEADDR`, and some datagrams meant for `mb_server_demo` reached EasyModbus. Its holding registers 2..4 showed our
test pattern (`0xA4A5…`). On Windows, two UDP servers can share a port, and binding one of them to `127.0.0.1` doesn't
separate them reliably. Test servers should use a different port (`--port 1502`).

## Linux results (WSL Ubuntu 24.04, g++, port 1502)
0 warnings; `mb_client_test` TCP and UDP PASS (including poller and split checks); EtherNet/IP regression PASS.

## Later (recorded, not in this phase)
- `ModbusClientPoller` as a `FieldbusDevice` transport and in `SoftFieldbus.Net` (C#).
- Modbus RTU (USB-RS485) for the client and the server.
- A Dev.3 `IODevice` adapter DLL (`fIODevices`, INI-compatible with `IOModBus.ini`), so ProInspect can switch from `UvcIOModBus` without code changes.
