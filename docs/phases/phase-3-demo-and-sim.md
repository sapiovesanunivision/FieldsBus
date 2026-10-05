# Phase 3 — Demo device & PLC simulator

**Goal:** test the adapter end-to-end without real hardware.

## Files
- `examples/eip_adapter_demo.cpp`
- `tools/eip_scanner_sim.cpp`

## Tasks
- [ ] Demo: inputs = echo of outputs, bytes 0..3 = heartbeat counter, prints output changes, Ctrl+C
- [ ] Simulator: ListIdentity, RegisterSession, read product name, Forward_Open, cyclic I/O, echo check, Forward_Close
- [ ] Simulator options for negative tests (sizes, RPI, `--skip-close`)
- [ ] Run happy path (exit code 0)
- [ ] Negative tests: wrong size → 0x0127/0x0128, fast RPI → 0x0111, second owner → 0x0106, watchdog timeout

## Done when
- All tests above pass on Linux build
