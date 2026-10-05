# Phase 2 — Connections & cyclic I/O

**Goal:** a PLC can open a class-1 exclusive-owner connection and exchange cyclic I/O.

## Files
- `src/eip_adapter.cpp`

## Tasks
- [ ] Path parser: logical segments (8/16-bit), electronic key 0x34, port segments, data segment 0x80
- [ ] Forward_Open / Large_Forward_Open with validation and CM extended status codes
- [ ] Connection types: exclusive owner, input-only (198), listen-only (199)
- [ ] Forward_Close, Unconnected_Send (0x52) unwrapping
- [ ] T→O port from Sockaddr Info item 0x8001
- [ ] Class-1 consume: CPF parse, source IP check, sequence-count duplicate filter, run/idle header
- [ ] Class-1 produce at RPI (catch-up / resync when late)
- [ ] Inactivity watchdog (multiplier × RPI, 10 s initial grace), owner loss → run=false, close listen-only
- [ ] Identity status word reflects owned / run / idle

## Done when
- Builds clean; verified end-to-end in Phase 3
