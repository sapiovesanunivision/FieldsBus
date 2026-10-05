# Phase 3 — Demo device & PLC simulator

**Goal:** test the adapter end-to-end without real hardware.

## Files
- `examples/eip_adapter_demo.cpp`
- `tools/eip_scanner_sim.cpp`

## Tasks
- [x] Demo: inputs = echo of outputs, bytes 0..3 = heartbeat counter, prints output changes, Ctrl+C
- [x] Simulator: ListIdentity, RegisterSession, read product name, Forward_Open, cyclic I/O, echo check, Forward_Close
- [x] Simulator options for negative tests (sizes, RPI, `--skip-close`)
- [x] Run happy path (exit code 0)
- [x] Negative tests: wrong size → 0x0127/0x0128, fast RPI → 0x0111, second owner → 0x0106, watchdog timeout

## Done when
- All tests above pass on Linux build

## Results (Linux, loopback, RPI 10 ms)
| Test | Expected | Result |
|---|---|---|
| Happy path, 5 s | ~500 T→O, echo, Forward_Close OK | 500/500 T→O, 497 echo matches, PASS |
| O→T size 30 (adapter 32) | 0x0127 | 0x0127 |
| T→O size 16 (adapter 32) | 0x0128 | 0x0128 |
| RPI 1 ms (min 2 ms) | 0x0111 | 0x0111 |
| O→T instance 155 | 0x0117 | 0x0117 |
| Second exclusive owner | 0x0106 | 0x0106 |
| Input-only (198, O→T size 6) | accepted | accepted, 100 T→O/s |
| `--skip-close` | watchdog closes after 8×RPI | closed ~80 ms after last packet |
