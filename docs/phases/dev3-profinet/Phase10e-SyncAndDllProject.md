# Phase 10e: Sync and DLL project with both raw-Ethernet backends

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repositories: **FieldsBus**
> (step 10e.1) and **Dev.3** `dev/4516/4526-ProcessImage`. **Status: done 2026-10-21**: FieldsBus `baaead4`
> (pushed), Dev.3 `b2d5f687c6` "Sync FieldsBus baaead4" (local, not pushed). The blocks below are the files.

## Goal
Bring softpn into Dev.3's `SoftFieldbus145_x64(d).dll`, with **both** raw-Ethernet backends compiled in: the
softpn_ndis driver and Npcap. Npcap stays a real option until the driver is signed (D5), so the Npcap SDK becomes a
build prerequisite for Dev.3.

## Documentation References
- [dev3-integration.md](../../dev3-integration.md): mirror layout and sync rules
- [Phase 8](../phase-8-dev3.md) 8b: how the DLL project was created
- [Phase 9.20](../profinet/Phase9.20-NdisBackend.md): `l2_port_windows.cpp`, `SOFTPN_HAS_NPCAP`

---

## Step 10e.1: Fix the sync script (FieldsBus)

**File:** `tools/sync-to-dev3.ps1`

### Purpose
The script maps `drivers/softpn_ndis/inc`, but `git archive` exports only `include` and `src`. The first sync with
softpn therefore stops with "HEAD has no drivers/softpn_ndis/inc". The uncommitted-changes check has the same gap.
Commit this fix with 10a-10d, **before** the sync.

### Implementation

```powershell
# ---- FieldsBus state ----  (line ~86)
$dirty = Invoke-Git @('status', '--porcelain', '--', 'include', 'src', 'drivers/softpn_ndis/inc')
if ($dirty -and -not $AllowDirty) {
    throw "FieldsBus include/, src/ or drivers/softpn_ndis/inc has uncommitted changes (the sync copies HEAD). Commit them, or pass -AllowDirty.`n$($dirty -join "`n")"
}

# ---- Export HEAD ----  (line ~105): every Src folder of $mappings must be in the archive
    Invoke-Git @('archive', '--format=zip', "--output=$zip", 'HEAD', 'include', 'src', 'drivers/softpn_ndis/inc') | Out-Null
```

Also update the help text above `param()` (lines 20-22). The Dev.3 project now compiles all three backends, as follows:

```text
    The Dev.3 vcxproj compiles src\pn\l2_port_windows.cpp, l2_port_ndis.cpp and l2_port_npcap.cpp with
    SOFTPN_HAS_NPCAP=1 (the Npcap SDK is required there, see Phase 10e), and adds drivers\softpn_ndis\inc and
    $(NPCAP_SDK)\Include to the include path.
```

**Done 2026-10-21:** implemented with one list, `$exportPaths`, used by both the check and the export.
`-WhatIf` against Dev.3 with PowerShell 7 and 5.1 at `366adbf`: added 32, changed 5, removed 0, unchanged 14, and
nothing written to Dev.3.

### Key Concepts
1. **Why it was not seen before**: the mapping was added in Phase 9.20, but the script has not been run against Dev.3
   since then (the last sync is `b547c2b`).
2. **Dry run result with the fix** (FieldsBus `65cb2a0`): **added 32, changed 5, removed 0, unchanged 14**. The
   counts grow with 10a-10d (no new files; `fieldbus_device.*` and `l2_port_windows.cpp` change).

---

## Step 10e.2: Run the sync

### Purpose
Copy the committed FieldsBus HEAD into `Dev\Sdk\UvX` and write the stamp.

### Procedure

```powershell
# FieldsBus: 10a-10d and 10e.1 committed and pushed (the script warns about unpushed commits)
pwsh D:\Univision\Misc\FieldsBus\tools\sync-to-dev3.ps1 -Dev3Root D:\Univision\Dev.3 -WhatIf   # preview
pwsh D:\Univision\Misc\FieldsBus\tools\sync-to-dev3.ps1 -Dev3Root D:\Univision\Dev.3
```

Expected:
- **added**: `include\softpn\l2_port.hpp`, `pn_device.hpp`; 29 files in `sources\SoftFieldbus\src\pn\`;
  `sources\SoftFieldbus\drivers\softpn_ndis\inc\softpn_ndis_ioctl.h`
- **changed**: `softeip\bytes.hpp`, `export.hpp`, `periodic_timer.hpp`, `softfb\fieldbus_device.hpp`,
  `src\fieldbus_device.cpp`
- the warning "Source files were added or removed: update ... SoftFieldbus1x.vcxproj" (next steps)

Dev.3: stage only `Dev\Sdk\UvX` (not the untracked AE1931 files). Commit after step 10e.4 builds, message
`Sync FieldsBus <hash>` (the script prints it).

### Key Concepts
1. **The mirror is read-only in Dev.3**: if a build error points into `src\pn`, fix it in FieldsBus and sync again.

---

## Step 10e.3: Project file

**File:** `Dev/Sdk/UvX/sources/SoftFieldbus/SoftFieldbus1x.vcxproj`

### Purpose
Compile softpn in every configuration with both backends. If `NPCAP_SDK` is missing, the build must stop with a clear
message. Otherwise the DLL would build without Npcap and fail only at a customer's site.

### Implementation

Header comment (top of the file), replace the second sentence:

```xml
  src\*.cpp, src\pn\*, drivers\softpn_ndis\inc\*.h and ..\..\include\softeip|softmb|softfb|softpn are a MIRROR of
  github.com/sapiovesanunivision/FieldsBus, written by its tools\sync-to-dev3.ps1 (see FIELDSBUS_SYNC.txt). Do not
  edit them here: fix them in FieldsBus and sync.
  PROFINET (softpn) needs the Npcap SDK: environment variable NPCAP_SDK (https://npcap.com/#download).
```

Insert **before** `<Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />`:

```xml
  <!-- PROFINET (softpn): raw Ethernet through the softpn_ndis driver or Npcap, chosen at run time
       (service: Parameters\profinet\L2Backend). Npcap stays needed until the driver is signed, so the Npcap SDK
       is required on every build PC and on the build server. -->
  <Target Name="SoftFieldbusCheckNpcapSdk" BeforeTargets="PrepareForBuild">
    <Error Condition="'$(NPCAP_SDK)' == '' or !Exists('$(NPCAP_SDK)\Include\pcap.h')"
           Text="SoftFieldbus PROFINET: set the environment variable NPCAP_SDK to the Npcap SDK folder (https://npcap.com/#download), then restart Visual Studio" />
  </Target>
  <ItemGroup>
    <ClCompile Include="src\pn\l2_port_memory.cpp" />
    <ClCompile Include="src\pn\l2_port_windows.cpp" />
    <ClCompile Include="src\pn\l2_port_ndis.cpp" />
    <ClCompile Include="src\pn\l2_port_npcap.cpp" />
    <ClCompile Include="src\pn\pn_ar.cpp" />
    <ClCompile Include="src\pn\pn_blocks.cpp" />
    <ClCompile Include="src\pn\pn_cyclic.cpp" />
    <ClCompile Include="src\pn\pn_dcp.cpp" />
    <ClCompile Include="src\pn\pn_device.cpp" />
    <ClCompile Include="src\pn\pn_frame.cpp" />
    <ClCompile Include="src\pn\pn_lldp.cpp" />
    <ClCompile Include="src\pn\pn_model.cpp" />
    <ClCompile Include="src\pn\pn_rpc.cpp" />
    <ClCompile Include="src\pn\pn_rta.cpp" />
    <ClCompile Include="src\pn\pn_station_store.cpp" />
    <!-- mirrored for completeness, Linux only -->
    <None Include="src\pn\l2_port_linux.cpp" />
  </ItemGroup>
  <ItemGroup>
    <ClInclude Include="..\..\include\softpn\l2_port.hpp" />
    <ClInclude Include="..\..\include\softpn\pn_device.hpp" />
    <ClInclude Include="src\pn\l2_port_windows.hpp" />
    <ClInclude Include="src\pn\pn_ar.hpp" />
    <ClInclude Include="src\pn\pn_blocks.hpp" />
    <ClInclude Include="src\pn\pn_cyclic.hpp" />
    <ClInclude Include="src\pn\pn_dcp.hpp" />
    <ClInclude Include="src\pn\pn_defs.hpp" />
    <ClInclude Include="src\pn\pn_frame.hpp" />
    <ClInclude Include="src\pn\pn_lldp.hpp" />
    <ClInclude Include="src\pn\pn_model.hpp" />
    <ClInclude Include="src\pn\pn_rpc.hpp" />
    <ClInclude Include="src\pn\pn_rta.hpp" />
    <ClInclude Include="src\pn\pn_station_store.hpp" />
    <ClInclude Include="src\pn\pn_uuid.hpp" />
    <ClInclude Include="drivers\softpn_ndis\inc\softpn_ndis_ioctl.h" />
  </ItemGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>$(NPCAP_SDK)\Include;drivers\softpn_ndis\inc;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>SOFTFB_HAS_PROFINET=1;SOFTPN_HAS_NPCAP=1;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
    <Link>
      <AdditionalDependencies>$(NPCAP_SDK)\Lib\x64\wpcap.lib;iphlpapi.lib;delayimp.lib;%(AdditionalDependencies)</AdditionalDependencies>
      <!-- loaded on first use from System32\Npcap: a PC with only the driver (or nothing) gets an error message, not a crash -->
      <DelayLoadDLLs>wpcap.dll;%(DelayLoadDLLs)</DelayLoadDLLs>
    </Link>
  </ItemDefinitionGroup>
```

### Key Concepts
1. **One `ItemDefinitionGroup` without a condition** adds to the four per-configuration groups above it (`%(...)`
   keeps their values). It must come after them, which is the case right before `Microsoft.Cpp.targets`.
2. **`SOFTFB_HAS_PROFINET` only in the DLL**: `fieldbus_device.hpp` always declares `Transport::ProfinetRt` and
   `ProfinetOptions`, so the service and the PLC simulator need no define.
3. **Delay load**: `wpcap.dll` is resolved only when the Npcap backend is used. The DLL loads on a PC without
   Npcap, and the driver backend works there.
4. **`_WIN32_WINNT=0x0601`** stays. softpn needs Vista+ APIs only (`GetIfEntry2`), the same as in FieldsBus.

---

## Step 10e.4: Filters file

**File:** `Dev/Sdk/UvX/sources/SoftFieldbus/SoftFieldbus1x.vcxproj.filters`

### Purpose
Groups the softpn files under a "pn" folder in Solution Explorer. Append the block before `</Project>`.

### Implementation

```xml
  <ItemGroup>
    <Filter Include="pn">
      <UniqueIdentifier>{6E2B5C1A-3D4F-4A8B-9C7E-1F2A3B4C5D6E}</UniqueIdentifier>
    </Filter>
  </ItemGroup>
  <ItemGroup>
    <ClCompile Include="src\pn\l2_port_memory.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\l2_port_windows.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\l2_port_ndis.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\l2_port_npcap.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_ar.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_blocks.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_cyclic.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_dcp.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_device.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_frame.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_lldp.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_model.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_rpc.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_rta.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <ClCompile Include="src\pn\pn_station_store.cpp">
      <Filter>pn</Filter>
    </ClCompile>
    <None Include="src\pn\l2_port_linux.cpp">
      <Filter>pn</Filter>
    </None>
  </ItemGroup>
  <ItemGroup>
    <ClInclude Include="..\..\include\softpn\l2_port.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="..\..\include\softpn\pn_device.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\l2_port_windows.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_ar.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_blocks.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_cyclic.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_dcp.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_defs.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_frame.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_lldp.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_model.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_rpc.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_rta.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_station_store.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="src\pn\pn_uuid.hpp">
      <Filter>pn</Filter>
    </ClInclude>
    <ClInclude Include="drivers\softpn_ndis\inc\softpn_ndis_ioctl.h">
      <Filter>pn</Filter>
    </ClInclude>
  </ItemGroup>
```

### Key Concepts
1. **The GUID is arbitrary** but must be unique in the file. Any new GUID works.

---

## Step 10e.5: Build the DLL

### Procedure
- Set `NPCAP_SDK` (for example `D:\SDK\npcap-sdk-1.15`) and restart Visual Studio.
- Build `SDK\UvX\SoftFieldbus` in **Debug_2026** (code analysis), **Release_2026**, **Debug_2022**, **Release_2022**.
- `dumpbin /dependents SoftFieldbus145_x64.dll`: `IPHLPAPI.DLL` listed, `wpcap.dll` only under "delay load".
- Temporarily clear `NPCAP_SDK` in a console and run `msbuild`: the build stops with the Npcap message.
- Commit in Dev.3: `Sync FieldsBus <hash>` (mirror + vcxproj + filters).

---

## Verification Checklist

Results 2026-10-21 (dev PC, VS 2026 MSBuild, `NPCAP_SDK` = Npcap SDK 1.16):

- [x] FieldsBus: sync-script fix committed with 10a-10d and pushed (`baaead4`)
- [x] Sync: added 32, changed 5, removed 0, unchanged 14; stamp `baaead4`
- [x] SoftFieldbus Debug_2026, Release_2026, Debug_2022, Release_2022 (Rebuild): 0 warnings, 0 errors;
      `RunCodeAnalysis` on in Debug
- [x] `dumpbin /dependents` (Release_2026): `IPHLPAPI.DLL` imported, `wpcap.dll` under delay load
- [x] `NPCAP_SDK` empty: "error : SoftFieldbus PROFINET: set the environment variable NPCAP_SDK ..." (vcxproj line 193)
- [x] `UvcIOSoftModBus`, `UnitTestUvcIOSoftModBus`, the service and the PLC simulator, Debug_2026 / Release_2026: 0 warnings
- [x] `UnitTestUvcIOSoftModBus` 11/11 in Release_2026 and Debug_2026 (vstest with `bin` on PATH: the test DLL is in
      `bin.test`, its dependencies in `bin`)
- [x] Dev.3 commit `b2d5f687c6` (40 files under `Dev\Sdk\UvX` only)

---

## Next Phase
Continue to [Phase10f-ByteOrder.md](Phase10f-ByteOrder.md): byte order for all transports.
