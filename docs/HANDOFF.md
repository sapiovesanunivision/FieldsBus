# Handoff: build & verify on Windows (VS 2026)

This is for the next session (human or Claude Code) that picks the work up **on a Windows machine with
Visual Studio 2026**. The previous session ran in a Linux container. It had no MSVC and no C++/CLI, so the
Windows-specific parts have **never been compiled**.

## 1. Context

- **Goal:** a software-only EtherNet/IP *adapter* (I/O device) in C++ that runs inside our Windows apps.
  It replaces a Hilscher card when cycle times of 10 ms or more are good enough. The PLC is the scanner/master.
- **Repository:** https://github.com/sapiovesanunivision/FieldsBus
- **Branch:** `claude/software-fieldbus-solution-yp9gxc`
  (https://github.com/sapiovesanunivision/FieldsBus/tree/claude/software-fieldbus-solution-yp9gxc).
  Commit there and don't open a PR unless asked.
  ```bat
  git clone -b claude/software-fieldbus-solution-yp9gxc https://github.com/sapiovesanunivision/FieldsBus.git
  cd FieldsBus
  ```
- **Read first:**
  - `README.md`: what is implemented, PLC setup, usage
  - `docs/PLAN.md`: the original plan and its follow-ups
  - `docs/phases/phase-*.md`: the per-phase checklists. Phase 5 has unchecked Windows items.

## 2. Current state

| Part | Status |
|---|---|
| Native lib `softeip` (static, or shared with `-DSOFTEIP_SHARED=ON`) | ✅ builds clean with g++ on Linux. End-to-end and negative tests pass (`docs/phases/phase-3-demo-and-sim.md`) |
| `eip_adapter_demo`, `eip_scanner_sim` | ✅ Linux |
| Windows code paths in `include/softeip/socket_compat.hpp` (Winsock, `timeBeginPeriod`, `SIO_UDP_CONNRESET`, thread priority) | ✅ VS 2026, 0 warnings; loopback end-to-end + negative tests pass (2026-10-05) |
| `CMakePresets.json` / `generate_vs2026.bat` (generator `Visual Studio 18 2026`) | ✅ works; VS 2026 writes **`SoftFieldbus.slnx`** |
| `dotnet/` C++/CLI wrapper `SoftEip.Net.dll` + C# sample `SoftEipSample` | ✅ builds (Release + Debug, 0 warnings); C# sample PASS 500/500 |
| Real PLC test | ❌ not done |
| **Phase 6:** `softmb` Modbus TCP/UDP slave, `softfieldbus` (`FieldbusDevice`), `mb_*` / `fb_device_demo` tools | ✅ Linux: all tests pass, pymodbus interop OK. ⚠️ never compiled with MSVC |
| **Phase 6:** `SoftFieldbus.Net.dll` + C# `SoftFieldbusSample` | ⚠️ written, **never compiled**; follows the Windows-verified `SoftEip.Net` pattern |

> **Update (2026-10-05, Windows session):** steps 1–6 below are done. The results and the six fixes are in
> `docs/phases/phase-5-dotnet-wrapper.md` → "Windows results". The most important finding is that Windows 11
> **ignores `timeBeginPeriod` for processes without a visible window**. `SocketLibrary` now opts out of that
> throttling; without it, RPI 10 ms delivered only ~85 % of the packets. The next task is §5.1, the real PLC test.

## 3. Steps

Run these from a **Developer Command Prompt for VS 2026** in the repo root.

### Step 1: tooling
```bat
cmake --version
dotnet --list-sdks
```
- CMake must be **4.2 or newer** for the `Visual Studio 18 2026` generator. If it is older, install a newer
  CMake, or fall back to `cmake --preset vs2022`.
- The .NET 8 SDK (or 10) is needed for the C# sample. In the VS Installer, the
  "C++/CLI support for v14x build tools" component must also be installed.

### Step 2: generate the solution
```bat
generate_vs2026.bat
```
This should produce `build\vs2026\SoftFieldbus.slnx` with the projects `softeip`, `eip_adapter_demo`,
`eip_scanner_sim`, `SoftEip.Net` and `SoftEipSample`.

### Step 3: build the native targets first
```bat
cmake --build --preset vs2026-release --target softeip eip_adapter_demo eip_scanner_sim
```
Fix every error and every **warning** (`/W4`). Keep the code portable: it must still build on Linux.

### Step 4: native end-to-end test
```bat
:: terminal 1
build\vs2026\bin\Release\eip_adapter_demo.exe
:: terminal 2
build\vs2026\bin\Release\eip_scanner_sim.exe --target 127.0.0.1 --rpi-ms 10 --seconds 5 --local-port 2223
```
- Expect `RESULT: PASS`, about 500 T→O packets, and echo matches.
- Accept the Windows Firewall prompt (TCP/UDP 44818, UDP 2222).
- Then repeat the negative tests listed in `docs/phases/phase-3-demo-and-sim.md` and compare the codes.

### Step 5: .NET wrapper + C# sample
```bat
cmake --build --preset vs2026-release --target SoftEip.Net
:: the C# project needs a NuGet restore; easiest is to build the whole solution in the VS IDE,
:: or from the command line:
msbuild build\vs2026\SoftFieldbus.slnx /restore /p:Configuration=Release /p:Platform=x64
```
Then run `build\vs2026\bin\Release\SoftEipSample.exe` together with the simulator from step 4. Expect `RESULT: PASS`.

### Step 6: record results and commit
- Tick the remaining items in `docs/phases/phase-5-dotnet-wrapper.md`.
- Add a "Windows results" table, like the one in phase 3, with timings and any jitter you observe.
- Commit to the branch with a descriptive message.

### Phase 6 on Windows (Modbus + FieldbusDevice + SoftFieldbus.Net)
1. Regenerate (`generate_vs2026.bat`) and build the whole solution: new targets `softmb`, `softfieldbus`,
   `mb_slave_demo`, `mb_master_sim`, `fb_device_demo`, `SoftFieldbus.Net`, `SoftFieldbusSample`. 0 warnings.
2. Native Modbus (port 502 needs no admin on Windows; allow TCP+UDP 502 in the firewall):
   ```bat
   build\vs2026\bin\Release\mb_slave_demo.exe
   build\vs2026\bin\Release\mb_master_sim.exe --transport tcp
   build\vs2026\bin\Release\mb_master_sim.exe --transport udp
   ```
   Expect `RESULT: PASS` for both.
3. One app on every transport:
   ```bat
   fb_device_demo.exe --transport eip              + eip_scanner_sim.exe --in-size 64 --out-size 64 --local-port 2223
   fb_device_demo.exe --transport modbus --unit 1  + mb_master_sim.exe --transport tcp  (then udp)
   ```
4. .NET: `SoftFieldbusSample.exe eip`, then `SoftFieldbusSample.exe modbus`, each with the matching simulator. Expect PASS.
5. Record the results in `docs/phases/phase-6-modbus.md` (tick 6c, add a "Windows results" table) and commit.

## 4. Likely trouble spots (and the intended fix)

| Symptom | Where | Fix |
|---|---|---|
| `D8016 '/clr' and '/RTC1'` or `/EHs` incompatible | `dotnet/CMakeLists.txt` strips `/EHsc` and `/RTC1` from the directory flags | Check the generated `SoftEip.Net.vcxproj`. If the flags are still there, set `BasicRuntimeChecks=Default` / `ExceptionHandling=Async` via `VS_GLOBAL_*` or `set_source_files_properties(... COMPILE_OPTIONS)` |
| `/clr:netcore` not applied / `TargetFramework` missing | `COMMON_LANGUAGE_RUNTIME "netcore"` + `DOTNET_TARGET_FRAMEWORK` | These need CMake 3.26 or newer. Check `<CLRSupport>NetCore</CLRSupport>` in the vcxproj |
| `/permissive-` vs `/clr` errors | Should not happen: warning flags are `PRIVATE` to the native targets | Make sure nothing adds them to `SoftEip.Net` |
| `C3923` (local class in a ref-class member) or `C3845` (member initializer in a ref class) | `dotnet/SoftEipNet.*` | Already avoided: the lambdas live in the free function `installCallbacks`. Keep it that way |
| `<thread>` / `<mutex>` "not supported with /clr" | Any header included by `SoftEipNet.h` | The public header must stay free of them. That's what the pimpl in `eip_adapter.hpp` is for |
| `SoftEipSample` can't find `SoftEip.Net.dll`, or `project.assets.json` missing | Generated `build\vs2026\dotnet\sample\SoftEipSample.csproj` (template `dotnet/sample/SoftEipSample.csproj.in`) | Build `SoftEip.Net` first, and restore NuGet (`/restore` or the IDE). Edit the **template**, not the generated file |
| `include_external_msproject` platform or config mismatch | `dotnet/CMakeLists.txt` | In the Configuration Manager, check that SoftEipSample maps to `x64`. Fall back to `SOFTEIP_DOTNET_SAMPLE=OFF` + `dotnet build` |
| `FileNotFoundException: Ijwhost.dll` at runtime | .NET 8/10 C++/CLI | `Ijwhost.dll` must be next to `SoftEip.Net.dll` in `bin\<Config>` |
| `BadImageFormatException` | C# app is AnyCPU or x86 | The app must be **x64** |
| `cannot bind TCP 44818` | Another EtherNet/IP stack is running: RSLinx, a Hilscher driver, a second demo | Stop it, or use `BindAddress` / `--bind` with a specific NIC IP |
| Same /clr issues in `SoftFieldbus.Net` | `dotnet/SoftFieldbusNet.*`, `softfb_clr_assembly()` in `dotnet/CMakeLists.txt` | Fix it the way `SoftEip.Net` was fixed. Both assemblies share the CMake function, so the fix covers both |
| `SoftFieldbusSample` can't load `SoftFieldbus.Net.dll` | template `dotnet/fieldbus_sample/SoftFieldbusSample.csproj.in` | Keep `<Private>true</Private>`; it's the same deps.json issue as SoftEipSample |
| `cannot bind TCP 502` | another Modbus server (an OPC/Modbus gateway, a second demo) | Stop it, or use another `--port` / `ModbusPort` |
| `winsock2.h` / `windows.h` redefinition in MFC apps | Including order | Only `eip_adapter.hpp` is public, and it doesn't include winsock. Don't include `socket_compat.hpp` from app code |

## 5. Next tasks after Windows is green (in priority order)

1. **Real PLC test** (Studio 5000 Generic Ethernet Module, see the README). Measure RPI jitter at 10 ms and 4 ms.
2. **EDS file** for non-Rockwell scanners.
3. **Multicast T→O.**
4. **TCP/IP (0xF5) and Ethernet Link (0xF6) objects.**
5. **Class-3 explicit messaging.**
6. **PROFINET:** a feasibility spike of p-net + Npcap. Check the licence first (GPLv3 or commercial).

Keep the existing conventions:
- one phase doc per chunk of work, under `docs/phases/`, with a checklist and a results table
- C++17
- no warnings
- the Linux build must keep working
