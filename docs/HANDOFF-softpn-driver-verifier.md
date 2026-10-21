# Handoff: Driver Verifier run for softpn_ndis.sys

**Audience:** a new Claude Code session (or a developer) that runs and evaluates the Driver Verifier test of the
own NDIS driver. Read this first, then [HANDOFF-softpn-ndis-driver.md](HANDOFF-softpn-ndis-driver.md) (driver design,
D0-D4 results) and [Phase 9.20](phases/profinet/Phase9.20-NdisBackend.md).

**Done (2026-10-21):** standard Verifier pass clean, see
[softpn-driver-verifier-report.md](softpn-driver-verifier-report.md). It found and fixed two bugs: the `FrameTypeArray`
let NDIS drop unicast PROFINET frames (driver), and `l2_port_ndis.cpp` did not reopen the adapter after an unbind
(backend). PROFINET tests in the VMs now use the private switch adapter "PN" (10.10.10.1/2).

## Context

- `softpn_ndis.sys` (NDIS 6.30 protocol driver, derived from the NDISPROT sample) replaces Npcap for softpn,
  the in-house PROFINET device. Code: `drivers/softpn_ndis/` (`sys/`, `inc/softpn_ndis_ioctl.h`, `test/`,
  `upstream/` = unmodified sample as diff base), user-mode backend `src/pn/l2_port_ndis.cpp`.
- **Done:**
  - D2: `softpn_ndis_test selftest` 18/18.
  - D3: two VMs, DataRun at 8 ms.
  - **D4 passed 2026-10-19:** CODESYS on a physical I219 test PC, 32 ms and 8 ms with 0 aborts.
- **Not done: Driver Verifier.** The driver has never run under Verifier. Code analysis showed only the 6 warnings
  of the unmodified sample. This run is the next quality gate before D5 (signing / installer).
- Branch `feature/profinet-poc`, last pushed commit `ffbf631`. Uncommitted: `drivers/softpn_ndis/pack_kit.ps1` (copies
  `sym\softpn_ndis.pdb` into the kit). No commit / push without the user.

## Test bed

| Item | Value |
|---|---|
| Host | PC-PROVE 10.0.0.128 (Hyper-V, also runs CODESYS Control Win; shares `\\10.0.0.128\D`, `\\10.0.0.128\E`) |
| **VM under test** | **pndevA**: Windows 11, test signing on, Secure Boot off, driver installed. Adapter `Ethernet 3`, MAC 00:15:5d:00:80:01, IP 192.168.184.38 (DHCP, Default Switch) |
| Peer VM | **pndevB**: same setup, adapter `Ethernet 4`, MAC 00:15:5d:00:80:02, IP 192.168.184.74 |
| Kit | `\\10.0.0.128\D\softpn_ndis_kit` (driver package, `install.cmd`, `update.cmd`, `uninstall.cmd`, `verifier.cmd`, `softpn_ndis_test.exe`, `pn\` = PROFINET tools, `sym\softpn_ndis.pdb`); in the VMs copied to `C:\Univision\softpn_ndis_kit` |
| Installed driver | Debug x64 build of 2026-10-16 12:40 (hash starts `302C39AF7DD0`); `sym\softpn_ndis.pdb` matches it |

The Default Switch DHCP gives the VMs no gateway (ping 10.0.0.128 -> "transmit failed. General failure"). Fixed
2026-10-20 with a persistent route to the switch IP on PC-PROVE (192.168.176.1/16; it can change when PC-PROVE
reboots): `New-NetRoute -InterfaceAlias "Ethernet 3" -DestinationPrefix 0.0.0.0/0 -NextHop 192.168.176.1`.

Hyper-V drops 802.1Q-tagged frames: in the VMs run the PROFINET tools untagged (`pn_device_demo --no-vlan 1`,
`pn_controller_sim --vlan 0`).

## Chosen approach: crash dump inside pndevA

The user may install anything in pndevA (WinDbg, VS 2026). A kernel debugger cannot catch a crash of its own
machine, so the plan is **post-mortem**: Verifier bugchecks -> kernel memory dump -> after the reboot WinDbg in
pndevA opens `C:\Windows\MEMORY.DMP`. A live debugger is only needed if a dump is not enough.

**Optional live debugger** (later):
- Host: `Set-VMComPort -VMName pndevA -Number 1 -Path \\.\pipe\pndevA_dbg`, with the VM off.
- In the VM: `bcdedit /debug on` and `bcdedit /dbgsettings serial debugport:1 baudrate:115200`.
- WinDbg on the host: Attach to kernel -> COM, Pipe, Reconnect, port `\\.\pipe\pndevA_dbg`, baud 115200.
- Ask before installing WinDbg on PC-PROVE: it is the CODESYS PC.

## Steps (the user runs them; give exact commands)

1. **PC-PROVE:** `Checkpoint-VM -Name pndevA -SnapshotName "before verifier"`.
2. **pndevA, admin PowerShell:**

   ```powershell
   winget install Microsoft.WinDbg     # without winget: see below
   mkdir C:\Univision\softpn_ndis_kit\sym
   copy \\10.0.0.128\D\softpn_ndis_kit\sym\softpn_ndis.pdb C:\Univision\softpn_ndis_kit\sym\
   Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl CrashDumpEnabled 2   # kernel dump
   Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl Overwrite 1
   cd C:\Univision\softpn_ndis_kit
   .\verifier.cmd                      # verifier /standard /driver softpn_ndis.sys
   Restart-Computer
   ```

   WinDbg without winget (worked in pndevA on 2026-10-20):

   ```powershell
   $dir = "$env:TEMP\windbg"; New-Item $dir -ItemType Directory -Force | Out-Null; cd $dir
   Invoke-WebRequest "https://aka.ms/windbg/download" -OutFile windbg.appinstaller
   [xml]$ai = Get-Content .\windbg.appinstaller
   Invoke-WebRequest $ai.AppInstaller.MainBundle.Uri -OutFile windbg.msixbundle
   Add-AppxPackage .\windbg.msixbundle
   ```

3. **After the reboot:** `verifier /querysettings` (softpn_ndis.sys listed), `net start SoftPnNdis`,
   `verifier /query` (driver loaded).
4. **Load, under Verifier:**
   - `.\softpn_ndis_test selftest 0`, 5 times.
   - Full stack, 30 min:
     - pndevA: `pn\pn_device_demo --adapter "Ethernet 3" --name softpn-dev1 --in-size 32 --out-size 32 --no-vlan 1`
     - pndevB: `pn\pn_controller_sim --adapter "Ethernet 4" --name softpn-dev1 --seconds 1800 --vlan 0`
     - Expected: DataRun, `aborts 0`.
   - Hard exits: close `pn_device_demo` with the window **X** while reads are pending; kill it with Task Manager.
   - `net stop SoftPnNdis` / `net start SoftPnNdis`, 3 times (with and without an open handle).
   - **Unbind with an open handle (most likely bug area):** disable and re-enable `Ethernet 3` in Network
     Connections while `softpn_ndis_test recv 0 -s 60` runs; then the same while `pn_device_demo` runs.
   - Optional, a second pass with more checks (e.g. low-resources simulation, NDIS/WIFI verification) **only after
     the standard pass is clean**. Take the exact option names / flag values from `verifier /?` on the VM (not
     verified here), reboot, repeat.
5. **Bugcheck:** the VM shows a blue screen and reboots. In WinDbg in pndevA: File -> Open dump file
   `C:\Windows\MEMORY.DMP`, then:

   ```
   .sympath srv*C:\symbols*https://msdl.microsoft.com/download/symbols;C:\Univision\softpn_ndis_kit\sym
   .reload
   !analyze -v
   kb
   !verifier
   ```

   The user pastes the output. Map the stack to `drivers/softpn_ndis/sys/*.c`.
6. **No bugcheck:** `verifier /query` at the end (counters). Then `verifier /reset` and reboot.

## Where to look first if Verifier fires

| Area | Why | File |
|---|---|---|
| Unbind / pause while reads are pending | ref counting of the open context, `ndisprotShutdownBinding`, `ndisprotWaitForPendingIO`, `ndisprotCancelPendingReads` (sleeps 100 ms loops) | `ndisbind.c`, `recv.c` |
| Receive copy path (softpn change) | `NdisGetDataBuffer`, `NdisCopyFromNetBufferToNetBuffer` with offsets for the VLAN re-insert, every NBL returned at the end | `recv.c` `NdisprotReceiveNetBufferLists` |
| Queue trim / free | `ndisprotQueueReceiveNetBufferList`; the sample leaked a ref when paused (fixed) | `recv.c` |
| Spin lock at DISPATCH | the filter snapshot under `NPROT_ACQUIRE_LOCK(..., DispatchLevel)` | `recv.c`, `ntdisp.c` |
| Write cancel | `NdisprotCancelWrite`, send completion | `send.c` |
| IOCTLs | `ndisprotSetFilter` / `ndisprotGetStats` (METHOD_BUFFERED lengths) | `ntdisp.c` |
| Known harmless | `PartialCancelId` is a UCHAR shifted by 56 (always 0, cancel IDs are per binding) | `ntdisp.c` |

## Fix, build, deploy loop

- Build on the dev PC: `drivers\softpn_ndis\build.ps1`. It uses the WDK NuGet packages (`nuget restore` once), the
  64-bit MSBuild and Debug without Spectre libs. Code analysis: add `-p:RunCodeAnalysis=true` (MSBuild).
- Kit: `drivers\softpn_ndis\pack_kit.ps1 -Dest \\10.0.0.128\D\softpn_ndis_kit` (a Claude Code permission rule allows
  it). It now also copies `sym\softpn_ndis.pdb`: always use the PDB of the same build.
- In the VM: copy the kit, `update.cmd` (uninstall + install), checkpoint first.
- After driver changes: selftest 18/18 in the VM, then the VM full-stack test; ideally D4 again on the physical
  test PC (I219, 10.0.0.193) with CODESYS.

## Rules

- **Never** test signing, driver installs, Verifier or Secure Boot changes on the user's dev PC (10.0.0.168).
  Only the VMs or the separate physical test PC.
- Checkpoint before every risky step; the user runs installs / reboots.
- The user is often remote: send files with `SendUserFile`.
- Do not touch Dev.3 (`D:\Univision\Dev.3`).
- The Siemens S7 test runs in another chat ([HANDOFF-softpn-siemens.md](HANDOFF-softpn-siemens.md)). State
  2026-10-20: S7-1500 in DataRun at 32 ms, 0 aborts; the PLC's output pattern does not change. Not resolved, and
  probably a PLC-program issue; not part of this handoff.

## Done when

- Standard Verifier pass with the full load above: no bugcheck; `verifier /query` counters recorded.
- Results go into "D2 result" / a new "Driver Verifier result" section of
  [HANDOFF-softpn-ndis-driver.md](HANDOFF-softpn-ndis-driver.md) and the phase-9 results page.
