# Phase 10: PROFINET + byte order in the Dev.3 process-image service - step documents

Step-by-step documents for [phase-10-dev3-profinet.md](../phase-10-dev3-profinet.md), written with `/spec-to-code`.
Each sub-phase builds on the previous ones and takes about 30-60 minutes. The overview page holds the decisions
and the results tables.

- **Status: planned.** The code blocks are proposals, written against the sources of 2026-10-21 (Dev.3
  `8ab48fff1a` on `dev/4516/4526-ProcessImage`, FieldsBus `65cb2a0` on `feature/profinet-poc`). Nothing has been
  built or tested yet. Fix the blocks here when the real files differ.
- Paths `Dev/...` are relative to the Dev.3 root (`D:\Univision\Dev.3`). **`D3`** means
  `Dev/Applicazioni/ProInspect/ProVision/Sources/UvpProcessImage/ProInspectProcessImageSoftFieldbusService`.
- Never touch the untracked AE1931 files on the Dev.3 branch.

| Phase | Title | Repository | Files |
|---|---|---|---|
| **FieldsBus** | | | |
| [10a](Phase10a-L2BackendOption.md) | L2 backend option in `softfb::DeviceConfig` | FieldsBus | `fieldbus_device.hpp/.cpp`, `l2_port_windows.cpp`, `SoftFieldbusNet.h/.cpp`, `fb_device_demo.cpp` |
| [10b](Phase10b-ProInspectGsdml.md) | GSDML for the ProInspect service | FieldsBus | `GSDML-V2.43-Univision-ProInspect-20261021.xml`, `pn_selftest.cpp`, `CMakeLists.txt` |
| [10c](Phase10c-CodesysProgram.md) | CODESYS PLC program | FieldsBus | `ProInspectPn.st`, `ProInspectPn_PLCopen.xml` |
| [10d](Phase10d-TiaProgramAndGuide.md) | TIA PLC program and the PLC setup guide | FieldsBus | `ProInspectPn.scl`, `SoftFieldbusPnPLCSetup.md` |
| **Dev.3** | | | |
| [10e](Phase10e-SyncAndDllProject.md) | Sync and DLL project with both raw-Ethernet backends | Dev.3 | `SoftFieldbus1x.vcxproj` (+ `.filters`) |
| [10f](Phase10f-ByteOrder.md) | Byte order for all transports | Dev.3 | `ProcessImageByteOrder.h`, service config / settings / cycle / command line |
| [10g](Phase10g-ProfinetTransport.md) | PROFINET transport in the service | Dev.3 | `...Client.h/.cpp`, `...Service.cpp`, `...ServiceMain.cpp` |
| [10h](Phase10h-RegistryGsdmlInstaller.md) | Registry files, GSDML folder, installer | Dev.3 | `Registry\*.reg`, `GSDML\`, `.iss` |
| [10i](Phase10i-ReadmeAndPlcSim.md) | Service README and PLC simulator | Dev.3 | `README.md`, `ProInspectProcessImagePlcSim.cpp` |
| [10j](Phase10j-BuildAndTest.md) | Build and test | both | - |

## Order of work

```
FieldsBus                                   Dev.3 (dev/4516/4526-ProcessImage)
10a L2 backend option ─┐
10b ProInspect GSDML ──┼─ commit + push ──> 10e sync + vcxproj ──> 10f byte order ──> 10g PROFINET
10c CODESYS program    │                                                               │
10d TIA program, guide ┘                                         10h .reg, GSDML, installer
                                                                 10i README, PLC simulator
                                                                 10j build and test (both repos)
```

10c and 10d need only the GSDML of 10b. They can be written while the Dev.3 work goes on, and tested in 10j.
