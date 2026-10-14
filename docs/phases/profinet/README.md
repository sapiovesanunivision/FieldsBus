# softpn: PROFINET RT device PoC - phase documents

Step-by-step implementation of the in-house PROFINET RT IO-Device (Conformance Class A, RT_CLASS_1) for
Windows (Npcap) and Linux (AF_PACKET), generated with `/spec-to-code` from [the spec](../../profinet-spec.md).

**Every code block is the verified file from branch `feature/profinet-poc`**: built with 0 warnings
(MSVC `/W4`, `/analyze /WX` for `SoftFieldbus.dll`, g++ 13 `-Wall -Wextra -Wpedantic`) and tested with
`pn_selftest` (107 checks), a Linux veth test and a Windows Npcap test against the controller simulator.
Results: [phase-9-profinet.md](../phase-9-profinet.md).

The phases follow the include dependencies of the code, so each phase compiles on top of the previous
ones. The spec planned 24 phases; codecs and their users were merged where a file could not be split
without duplicating code, giving 18.

| Phase | Title | Files |
|---|---|---|
| **A - L2 access and timing** | | |
| [9.01](Phase9.01-BuildScaffolding.md) | Build scaffolding, constants and the L2 interface | `bytes.hpp`, `export.hpp`, `pn_defs.hpp`, `l2_port.hpp`, `pn_frame.hpp`, `pn_frame.cpp`, `l2_port_memory.cpp` |
| [9.02](Phase9.02-L2PortNpcap.md) | Npcap backend (Windows) | `l2_port_npcap.cpp` |
| [9.03](Phase9.03-L2PortLinux.md) | AF_PACKET backend (Linux) | `l2_port_linux.cpp` |
| [9.04](Phase9.04-Dcp.md) | DCP: discovery and naming | `pn_station_store.hpp`, `pn_station_store.cpp`, `pn_dcp.hpp`, `pn_dcp.cpp` |
| [9.05](Phase9.05-ProbeAndJitterGate.md) | pn_probe and the timing gate (Gate A) | `periodic_timer.hpp`, `pn_probe.cpp` |
| **B/C - LLDP, RPC, blocks, model** | | |
| [9.06](Phase9.06-Lldp.md) | LLDP transmitter | `pn_lldp.hpp`, `pn_lldp.cpp` |
| [9.07](Phase9.07-RpcCodecAndEndpoint.md) | DCE/RPC (connectionless) over UDP 34964 | `pn_uuid.hpp`, `pn_rpc.hpp`, `pn_rpc.cpp` |
| [9.08](Phase9.08-PnioBlocks.md) | PNIO blocks (Connect, Control, Read/Write, I&M0) | `pn_blocks.hpp`, `pn_blocks.cpp` |
| [9.09](Phase9.09-ModelAndGsdml.md) | Device model and GSDML | `pn_model.hpp`, `pn_model.cpp`, `GSDML-V2.43-Univision-SoftPN-20261013.xml` |
| **D - cyclic, alarms, AR, device** | | |
| [9.10](Phase9.10-CyclicEngine.md) | Cyclic RT_CLASS_1 data exchange | `pn_cyclic.hpp`, `pn_cyclic.cpp` |
| [9.11](Phase9.11-RtaAlarms.md) | Alarm transport (RTA) | `pn_rta.hpp`, `pn_rta.cpp` |
| [9.12](Phase9.12-ArStateMachine.md) | AR state machine | `pn_ar.hpp`, `pn_ar.cpp` |
| [9.13](Phase9.13-DeviceFacade.md) | softpn::Device and pn_device_demo (Gate B) | `pn_device.hpp`, `pn_device.cpp`, `pn_device_demo.cpp` |
| **E - test tools** | | |
| [9.14](Phase9.14-ControllerSimulator.md) | Controller simulator (pn_controller_sim) | `pn_controller.hpp`, `pn_controller.cpp`, `pn_controller_sim.cpp` |
| [9.15](Phase9.15-SelfTest.md) | Offline self-test (pn_selftest) | `pn_selftest.cpp` |
| **F - common API and .NET** | | |
| [9.16](Phase9.16-FieldbusDeviceTransport.md) | PROFINET in softfb::FieldbusDevice | `fieldbus_device.hpp`, `fieldbus_device.cpp`, `fb_device_demo.cpp`, `CMakeLists.txt` |
| [9.17](Phase9.17-DotNetWrapper.md) | PROFINET in SoftFieldbus.Net (C#) | `SoftFieldbusNet.h`, `SoftFieldbusNet.cpp`, `Program.cs` |
| **G - CODESYS bring-up** | | |
| [9.18](Phase9.18-CodesysBringUp.md) | Windows setup and CODESYS bring-up (Gates C and D) | - |

## Gates
- **A** (9.05): Windows TX timing on the station NIC - `pn_probe --jitter`.
- **B** (9.13): device found and named by CODESYS / Proneta / `pn_probe --identify`.
- **C** (9.18): CODESYS completes the startup up to ApplicationReady.
- **D** (9.18): green at 8/16/32 ms, 1 h soak with 0 aborts, STOP/RUN, cable pull.

## Licensing reminder
Free Npcap: development and this PoC, at most 5 systems. Shipping on customer machines needs Npcap OEM
(management decision pending). The `L2Port` interface keeps an NDISPROT-based backend possible.
