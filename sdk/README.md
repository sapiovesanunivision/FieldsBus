# SoftFieldbus SDK

Software fieldbus devices for a PC with a standard network card: **EtherNet/IP adapter**, **Modbus TCP/UDP
server and client**, **PROFINET RT IO-Device** (Conformance Class A), and one API for all of them
(`softfb::FieldbusDevice`). Windows x64 (Visual Studio 2026) and Linux x64 (GCC). Version: `VERSION.txt`.

## Contents

| Folder | |
|---|---|
| `include/` | Headers: `softeip/` (EtherNet/IP), `softmb/` (Modbus), `softpn/` (PROFINET), `softfb/` (FieldbusDevice) |
| `lib/windows-x64/Debug`, `Release` | Static libraries `softeip`, `softmb`, `softpn`, `softfieldbus` (/MDd, /MD) |
| `lib/windows-x64/shared/Debug`, `Release` | `SoftFieldbus.lib`, the import library of the DLL (own folder: Windows file names ignore case, so it would replace `softfieldbus.lib`) |
| `bin/windows-x64/Debug`, `Release` | `SoftFieldbus.dll`. Release also: the tools below and the C++ runtime (app-local) |
| `lib/linux-x64` | Static libraries `lib*.a` (position independent) and `libSoftFieldbus.so` |
| `bin/linux-x64` | The tools below |
| `cmake/SoftFieldbusConfig.cmake` | `find_package(SoftFieldbus)`: imported targets for both platforms |
| `examples/` | Sample sources, `CMakeLists.txt`, `CMakePresets.json` |
| `gsdml/` | PROFINET device descriptions (generic SoftPN device, ProInspect service) |
| `docs/` | Raw-Ethernet backends, PLC setup guides, PLC test programs |

`SoftFieldbus.dll` / `libSoftFieldbus.so` contain everything, PROFINET included. The static libraries are the same
code split by protocol.

## Build the samples

Requirements: CMake 3.21 or newer. Windows: Visual Studio 2026 (CMake 4.2+, bundled with it). Linux: g++ 9 or newer.

```bat
rem Windows (Developer PowerShell or any console with cmake in PATH)
cd examples
cmake --preset vs2026
cmake --build --preset vs2026-release
build\vs2026\bin\Release\fb_device_demo.exe --transport modbus-tcp --port 1502
```

```sh
# Linux
cd examples
cmake --preset linux
cmake --build --preset linux
./build/linux/bin/fb_device_demo --transport modbus-tcp --port 1502
```

Static libraries instead of the shared one: add `-DSOFTFIELDBUS_USE_SHARED=OFF` to the configure step. On Windows the
static PROFINET libraries also need the Npcap SDK (environment variable `NPCAP_SDK`); without it `fb_device_demo` and
`pn_device_demo` are skipped in that mode. The shared library has no such requirement.

| Sample | Shows |
|---|---|
| `eip_adapter_demo` | EtherNet/IP adapter: I/O assemblies, the PLC is the scanner |
| `mb_server_demo` | Modbus TCP/UDP server: the PLC is the client |
| `fb_device_demo --transport eip\|modbus-tcp\|modbus-udp\|profinet` | One application, any fieldbus (`softfb::FieldbusDevice`) |
| `pn_device_demo --adapter <NIC>` | PROFINET RT device directly on `softpn::Device` |

## Use it in your project

```cmake
find_package(SoftFieldbus REQUIRED CONFIG PATHS "<SDK folder>/cmake" NO_DEFAULT_PATH)
target_link_libraries(myapp PRIVATE SoftFieldbus::shared)   # or SoftFieldbus::softfieldbus, ::softeip, ...
```

Windows with the shared library: put `SoftFieldbus.dll` next to your program (the samples copy it after the build).
Linux: CMake writes the SDK's `lib/linux-x64` into the program's RPATH; for a deployed program copy
`libSoftFieldbus.so` next to it and link with `-Wl,-rpath,'$ORIGIN'`, or install it in a library folder.

## Run

- **EtherNet/IP and Modbus:** normal sockets. Allow the program in the firewall (EtherNet/IP: TCP/UDP 44818,
  UDP 2222; Modbus: the port you choose, 502 by default, which needs root on Linux).
- **PROFINET** sends raw Ethernet frames:
  - Windows: **Npcap** (https://npcap.com; free edition: at most 5 PCs, not redistributable, products need the OEM
    licence) or the **softpn_ndis** driver. Choose with `--backend npcap|ndis|auto`. See `docs/softpn-l2-backend.md`.
  - Linux: AF_PACKET, needs root or `sudo setcap cap_net_raw+ep <program>`.
  - Import a GSDML from `gsdml/` into the PLC's engineering tool; the device name and the module sizes must match
    the program's options (`pn_device_demo` prints them at start).

## Tools (`bin/`)

| Tool | |
|---|---|
| `pn_probe --list` | Network cards seen by the PROFINET backend; `--jitter` measures the send timing |
| `pn_device_demo`, `fb_device_demo`, `eip_adapter_demo`, `mb_server_demo` | The samples, prebuilt |
| `pn_controller_sim` | Minimal PROFINET IO-Controller, for a test without a PLC (second PC or NIC) |
| `eip_scanner_sim` | EtherNet/IP scanner (plays the PLC) |
| `mb_client` | Modbus client (plays the PLC) |

## Documentation

| File | |
|---|---|
| `docs/softpn-l2-backend.md` | Which raw-Ethernet backend to use (Npcap, softpn_ndis, Linux) |
| `docs/softpn-siemens-setup.md` | PROFINET with a Siemens S7-1200 / S7-1500 (TIA Portal) |
| `docs/SoftFieldbusPnPLCSetup.md`, `docs/SoftFieldbusEipPLCSetup.md` | PLC setup for the ProInspect process image (PROFINET, EtherNet/IP) |
| `docs/plc/` | PLC test programs (CODESYS PLCopen XML / ST, TIA SCL) |

Source and issues: github.com/sapiovesanunivision/FieldsBus (commit in `VERSION.txt`).
