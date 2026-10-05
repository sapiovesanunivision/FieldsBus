# Phase 1 — Foundation & discovery

**Goal:** a library skeleton that a PLC or engineering tool (RSLinx, Studio 5000 browse) can *discover*
and talk explicit CIP to, without any cyclic I/O yet.

## Files
- `include/softeip/bytes.hpp`, `include/softeip/socket_compat.hpp`
- `include/softeip/cip_defs.hpp`, `include/softeip/eip_adapter.hpp`
- `src/eip_adapter.cpp`
- `CMakeLists.txt`, `.gitignore`

## Tasks
- [ ] Byte reader/writer + Winsock/POSIX socket layer (1 ms timer, SIO_UDP_CONNRESET fix)
- [ ] Protocol constants (encapsulation, CPF, CIP services/classes/status)
- [ ] Public `Adapter` API (pimpl, thread-safe I/O image, callbacks)
- [ ] Network thread with `select()` loop; TCP 44818 listener, UDP 44818, UDP 2222 sockets
- [ ] Encapsulation: NOP, ListIdentity, ListServices, ListInterfaces, RegisterSession, UnRegisterSession, SendRRData
- [ ] CIP dispatcher: Identity (Get_Attributes_All / Get_Attribute_Single / Reset), Assembly get attr 3/4
- [ ] CMake build (MSVC + GCC), warnings clean

## Done when
- `cmake --build` passes with no warnings
- A UDP ListIdentity probe to port 44818 returns the configured product name
