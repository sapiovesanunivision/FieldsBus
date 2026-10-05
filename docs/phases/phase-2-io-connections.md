# Phase 2 — Connections & cyclic I/O

**Goal:** a PLC can open a class-1 exclusive-owner connection and exchange cyclic I/O.

## Files
- `src/eip_adapter.cpp`

## Tasks
- [x] Path parser: logical segments (8/16-bit), electronic key 0x34, port segments, data segment 0x80
- [x] Forward_Open / Large_Forward_Open with validation and CM extended status codes
- [x] Connection types: exclusive owner, input-only (198), listen-only (199)
- [x] Forward_Close, Unconnected_Send (0x52) unwrapping
- [x] T→O port from Sockaddr Info item 0x8001
- [x] Class-1 consume: CPF parse, source IP check, sequence-count duplicate filter, run/idle header
- [x] Class-1 produce at RPI (catch-up / resync when late)
- [x] Inactivity watchdog (multiplier × RPI, 10 s initial grace), owner loss → run=false, close listen-only
- [x] Identity status word reflects owned / run / idle

## Done when
- Builds clean; verified end-to-end in Phase 3
