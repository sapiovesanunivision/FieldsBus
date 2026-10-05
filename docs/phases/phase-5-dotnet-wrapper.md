# Phase 5 — .NET wrapper (C++/CLI) and DLL build

**Goal:** use the adapter from C# apps (WPF, WinUI, MAUI on Windows, services) through a
C++/CLI mixed-mode assembly that exposes a real .NET class, and optionally ship the native
library as a C++ DLL.

## Files
- `dotnet/SoftEipNet.h`, `dotnet/SoftEipNet.cpp`: `SoftFieldbus.EipAdapter`, `EipAdapterConfig`, event args
- `dotnet/CMakeLists.txt`: `SoftEip.Net` target (`/clr:netcore` or .NET Framework), C# sample wiring
- `dotnet/sample/Program.cs`, `dotnet/sample/SoftEipSample.csproj.in`: C# sample (generated csproj)
- `include/softeip/eip_adapter.hpp`: `SOFTEIP_API` export macro
- `CMakeLists.txt`: options `SOFTEIP_SHARED`, `SOFTEIP_BUILD_DOTNET`, `SOFTEIP_DOTNET_FRAMEWORK`; common `bin/` output folder

## Design decisions
- **One deployable assembly:** the static native lib is linked into `SoftEip.Net.dll`. For .NET 5+,
  `Ijwhost.dll` must sit next to it; it lands in the same `bin/<Config>` folder.
- **Native thread → events:** callbacks use `gcroot` and run on the network thread. Exceptions thrown by
  handlers are caught (a managed exception must never unwind into native frames) and reported through `Log`.
- **Lifetime:** the native `Adapter` is created in `Start()` and destroyed in `Stop()`. While it runs, the
  `gcroot` keeps the managed object alive. `Dispose()` stops it, and the finalizer is only a safety net.
- **/clr constraints handled:** no `<thread>`/`<mutex>` in public headers (pimpl), `/EHa` instead of `/EHsc`,
  no `/RTC1`, no lambdas in ref-class members (C3923), no NSDMI in ref classes (C3845).
- **Native DLL option** (`-DSOFTEIP_SHARED=ON`): exports `softeip::Adapter` with its C++ API. This needs the
  same compiler and runtime (/MD) as the consuming app.

## Tasks
- [x] `SOFTEIP_API` export macro + `SOFTEIP_SHARED` option (verified on Linux: shared build + end-to-end PASS)
- [x] C++/CLI wrapper: config, Start/Stop, SetInputs (also before Start), GetOutputs, PlcInRun, OutputConnected, events
- [x] CMake: `/clr:netcore` (`net8.0` default, `net10.0`) or .NET Framework (`v4.8`)
- [x] C# sample project added to the generated VS solution (`include_external_msproject`)
- [ ] **Build on Windows with VS 2026** (not possible in the Linux CI container: no MSVC / C++/CLI)
- [ ] Run `SoftEipSample` + `eip_scanner_sim` on Windows, expect `RESULT: PASS`

## Done when
- `generate_vs2026.bat` → solution builds `softeip`, `SoftEip.Net`, `SoftEipSample` with no errors
- `SoftEipSample.exe` + `eip_scanner_sim.exe --local-port 2223` → PASS
