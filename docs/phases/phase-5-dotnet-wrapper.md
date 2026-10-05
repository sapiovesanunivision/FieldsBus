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
- [x] **Build on Windows with VS 2026**: Release and Debug, whole solution, 0 errors and 0 warnings (`/W4`)
- [x] Run `SoftEipSample` + `eip_scanner_sim` on Windows, expect `RESULT: PASS`

## Done when
- `generate_vs2026.bat` → solution builds `softeip`, `SoftEip.Net`, `SoftEipSample` with no errors
- `SoftEipSample.exe` + `eip_scanner_sim.exe --local-port 2223` → PASS

## Windows results (2026-10-05)

**Machine and tools:** Windows 11 (10.0.26200), VS 2026 Enterprise (MSVC 19.51, toolset v145), Windows SDK
10.0.26100, CMake 4.4.0-rc2, .NET SDK 8.0.425 / 10.0.401.

**Setup:** loopback 127.0.0.1, with the adapter started via `--bind 127.0.0.1` (no firewall prompt). The
processes ran without a visible window (started from a script), which is the worst case for timer throttling.

| Test | Expected | Result |
|---|---|---|
| `cmake --preset vs2026` | solution with 5 projects | ✅ `build\vs2026\SoftFieldbus.slnx` (VS 2026 writes `.slnx`, not `.sln`) |
| Native build, Release + Debug, `/W4` | 0 errors, 0 warnings | ✅ after fixes 1 and 2 below |
| `SoftEip.Net` (C++/CLI, `/clr:netcore`, net8.0) | builds, `Ijwhost.dll` next to it | ✅ `CLRSupport=NetCore`, `/EHa`, no `/RTC1`; 0 warnings |
| Whole solution `msbuild … -restore`, Release + Debug | builds incl. `SoftEipSample` | ✅ 0 warnings |
| Happy path, native demo, RPI 10 ms, 5 s | ~500 T→O, echo, Forward_Close OK | ✅ 500/500, 499/500, 500/500 (echo 460–461), PASS. **Before fix 3: 425–429/500** |
| Happy path, `SoftEipSample` (C#), 5 s | PASS | ✅ 500/500 ×3 (echo 482–487), PASS. **Before fix 4: FileNotFoundException** |
| O→T size 30 (adapter 32) | 0x0127 | ✅ 0x0127 |
| T→O size 16 (adapter 32) | 0x0128 | ✅ 0x0128 |
| RPI 1 ms (min 2 ms) | 0x0111 | ✅ 0x0111 |
| O→T instance 155 | 0x0117 | ✅ 0x0117 |
| Second exclusive owner | 0x0106 | ✅ 0x0106; the first owner is unaffected (600/600, PASS) |
| Input-only (198, `--out-size 0`) | accepted | ✅ 199/200 T→O, PASS (after fix 5) |
| `--skip-close` | watchdog closes after 8×RPI | ✅ "connection timed out" ~0.17 s after the last *logged* output change (outputs are logged every 100 ms), consistent with 80 ms |
| Linux re-check (WSL Ubuntu 24.04, g++) | still builds and passes | ✅ 0 warnings; happy path 496/500 PASS, input-only PASS, RPI 1 ms → 0x0111 |

**Fixes made in the Windows session:**
1. `socket_compat.hpp`: `SIO_UDP_CONNRESET` was undeclared (C2065). SDK 10.0.26100 defines it in `<mswsock.h>`, which is now included.
2. `eip_adapter_demo.cpp`: C4996 on `std::localtime`. It now uses `localtime_s` on Windows and `localtime_r` elsewhere.
3. **Windows 11 timer throttling.** Windows 11 ignores `timeBeginPeriod(1)` for processes without a visible window (minimized HMI, service, a console started hidden) and falls back to the 15.6 ms tick: ~100 T→O/s for 3 s, then ~65/s. `SocketLibrary` now opts out with `SetProcessInformation(ProcessPowerThrottling, PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)`. The function is resolved at run time because the library targets `_WIN32_WINNT=0x0601`, and the call is a no-op where it doesn't exist.
4. `SoftEipSample.csproj.in`: the `SoftEip.Net` reference had `Private=false`. On .NET 5+ that drops it from `SoftEipSample.deps.json`, so the host refuses to load the app-local DLL. It now has `Private=true`; the copy is a no-op because both projects use the same output folder.
5. `eip_scanner_sim.cpp`: the PASS rule required `echoMatches > 0`, which an input-only connection can never meet. Echo is now required only when an echo is possible.
6. `eip_adapter_demo.cpp`: the 10 ms application cycle now uses a fixed period through `PeriodicTimer`. On Windows that's a periodic high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, Windows 10 1803+), falling back to a normal waitable timer. On other platforms it's `steady_clock` + `sleep_until`. Before, it was `sleep_for(10 ms)` after the work, which drifts.

**Jitter / notes:**
- **Echo matches:** with RPI 10 ms the simulator changes the output pattern every 100 ms. One echo per change can lag a cycle, so ~450–490 matches out of 500 is expected; it's not a loss. The C# sample's figure is a bit higher than the native demo's.
- **RPI jitter** was not measured per packet (the simulator only counts). Measure it with a real PLC (next task).
- **Firewall:** binding to 127.0.0.1 avoids the prompt. On a real NIC, accept TCP/UDP 44818 and UDP 2222.
