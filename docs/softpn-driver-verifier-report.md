# softpn_ndis.sys: Driver Verifier report

**Date:** 2026-10-20/21 · **Status:** standard pass done, with no bugcheck and no pool leak. Tests 1-8 pass, plus a
45-minute run on the fixed build. One real bug was found and fixed: NDIS dropped unicast PROFINET frames because of the
sample's `FrameTypeArray` (see [Bug found and fixed](#bug-found-and-fixed-ndis-dropped-unicast-profinet-frames-frametypearray)).
Once, pndevA received nothing after an adapter disable/enable; that was probably the same bug. A reboot fixed it, and it could not be reproduced afterwards. See
[Open issue](#open-issue-no-receive-after-adapter-disableenable-not-reproduced).

Context and plan: [HANDOFF-softpn-driver-verifier.md](HANDOFF-softpn-driver-verifier.md). Driver design and D0-D4:
[HANDOFF-softpn-ndis-driver.md](HANDOFF-softpn-ndis-driver.md).

## Setup

| Item | Value |
|---|---|
| Driver under test | `softpn_ndis.sys`, Debug x64 build of 2026-10-16 12:40 (`sym\softpn_ndis.pdb` of the same build) |
| VM under test | **pndevA** on PC-PROVE (Hyper-V, Windows 11, test signing): `Ethernet 3`, 00:15:5d:00:80:01, 192.168.184.38 |
| Peer VM | **pndevB**: `Ethernet 4`, 00:15:5d:00:80:02, 192.168.184.74 (driver installed, **no** Verifier) |
| Network | Hyper-V Default Switch, untagged (`--no-vlan 1` / `--vlan 0`; Hyper-V drops 802.1Q frames) |
| Verifier | `verifier /standard /driver softpn_ndis.sys`, flags `0x001209bb`, boot mode Persistent |
| Crash dump | `CrashDumpEnabled 2` (kernel), `Overwrite 1`; WinDbg installed in pndevA |
| Tools | `pn\pn_controller_sim.exe` of 2026-10-17 15:11 (with the WSAStartup fix), `pn_device_demo.exe` of 2026-10-16 |

Verifier standard flags active: special pool, force IRQL checking, pool tracking, I/O verification, deadlock
detection, DMA checking, security checks, miscellaneous checks, DDI compliance checking. Not active: low-resources
simulation, NDIS/WIFI verification (planned for the optional second pass).

## Results

No bugcheck in any test so far.

| # | Test | Result |
|---|---|---|
| 1 | `softpn_ndis_test selftest 0`, 5 times | **PASS**: 5 × 18/18 |
| 2 | Full stack, 30 min, 8 ms: `pn_device_demo` (pndevA) vs `pn_controller_sim --seconds 1800` (pndevB) | **PASS**: DCP, Connect, Write, Read I&M0, PrmEnd/ApplicationReady OK; cyclic sent 225,003 / received 225,003, echo matches 225,002, IOPS GOOD, Release OK |
| 3 | Hard exit: close `pn_device_demo` with the window X while in DataRun | **PASS**: selftest afterwards ALL PASS |
| 4 | Hard exit: `Stop-Process -Force` on `pn_device_demo` while in DataRun | **PASS**: selftest afterwards ALL PASS |
| 5 | `net stop` / `net start SoftPnNdis`, 3 times, no open handle | **PASS**: 3 × ALL PASS; `load: 4 / unload: 3`; pool back to 2 / 790 bytes |
| 6 | `net stop` with an open handle (`softpn_ndis_test recv 0 -s 60`) | **PASS**: SCM reports "could not be stopped" (unload deferred by the handle); `recv` runs its full 60 s and reads the stats; when the handle closes the deferred unload runs (`STOPPED`, `load: 4 / unload: 4`); `net start` + selftest ALL PASS |
| 7 | Unbind with an open handle (A): `Disable-NetAdapter` / `Enable-NetAdapter "Ethernet 3"` during `recv 0 -s 60` | **PASS** (no crash, no hang): the pending read completes with 995 (`ERROR_OPERATION_ABORTED`), stats still answer; after enable the binding is back (`list`, selftest ALL PASS) |
| 8 | Unbind during PROFINET traffic (B), fixed driver + reopen fix | **PASS**: no bugcheck; AR aborts (data hold), outputs STOP; after enable a new controller run reaches DataRun and PASS without restarting `pn_device_demo`; pool back to 2 / 790 bytes |

### Pool usage (test 2, `verifier /query`)

| | Start 03:48 | End 04:18 |
|---|---|---|
| Pool allocations attempted / succeeded (all special pool) | 88,896 | 229,594 |
| Allocations failed | 0 | 0 |
| Current NonPaged allocations / bytes | 2 / 790 | 2 / 790 |
| Peak NonPaged allocations / bytes | 216 / 25,614 | 216 / 25,614 |

About 140,000 allocations in 30 minutes (one receive copy per frame), and the current count stays flat at 2: no
leak. The peak (216, close to the receive-queue limit of 256) comes from the selftest, not from the cyclic traffic.
Unloads 3 and 4 passed Verifier's leak check on unload.

## Open issue: no receive after adapter disable/enable (not reproduced)

After test 7, pndevA receives no frames from pndevB:

- `pn_controller_sim` in pndevB: `DCP: no answer to Identify 'softpn-dev1'`, although `pn_device_demo` runs in
  pndevA (`WaitingForController`, `rx 0`).
- `softpn_ndis_test recv 0` in pndevA: 0 frames while the controller runs in pndevB.
- Ruled out so far:
  - **Driver state on pndevA:** the adapter is Up, the `UV_SoftPnNdis` binding is enabled, `list` shows it and the
    selftest passes. The selftest only sends and reads its own handle state, so it does not test receive from a peer.
  - **Hyper-V vNIC / switch:** pndevA reaches 192.168.176.1 and 10.0.0.128, and the ARP table has a fresh dynamic
    entry for pndevB (00-15-5d-00-80-02). Both VM adapters show `Connected True`, `Status Ok` on PC-PROVE.
  - **IP ping to pndevB fails:** probably the pndevB firewall, not conclusive.
- Code review of the rebind path found nothing obvious: on every open the packet filter (with ALL_MULTICAST, fallback:
  multicast list) is set again, and the receive path does not check the media-connect flag.
- Direction test with `softpn_ndis_test send 0 -n 100 -i 100` (0x88B5 broadcast): pndevA → pndevB works,
  pndevB → pndevA receives nothing. `net stop` / `net start SoftPnNdis` in pndevA does not help.
- **A reboot of pndevA fixes it.** Both directions work afterwards.
- **Not reproduced:** after the reboot, each trigger was repeated alone, each followed by the send/recv check. All
  passed:
  - driver reload;
  - driver reload with deferred unload (open handle);
  - adapter disable/enable;
  - adapter disable/enable with a pending read.
- Status: not attributed. Since a driver reload did not clear it, the stuck state was below our protocol (NDIS /
  Hyper-V vNIC). Watch for it in later runs; if it comes back, capture `!ndiskd.netadapter` / `!ndiskd.protocol`
  output from a live debugger before rebooting.

## Bug found and fixed: NDIS dropped unicast PROFINET frames (FrameTypeArray)

- **Symptom (after the reboot):** DCP and RPC work, but the AR aborts 25 ms after Data ("no cyclic frames from the
  controller (data-hold timer expired)", `rx 0`). With `softpn_ndis_test`, broadcast 0x88B5 arrives, but unicast to
  pndevA's MAC (0x8892 and 0x88B5) does not, although pndevB sends all of them (`tx 50 errors 0`).
- **pktmon in pndevA** (`pktmon filter add -m 00-15-5D-00-80-02`, `pktmon start --capture --comp nics`): each
  unicast frame reaches `netvsc.sys` and is dropped right there with **`DropReason Unsupported EtherType`**. The
  component list shows `softpn_ndis.sys` registered with the EtherTypes `VLAN` and `34958` (0x888E) only.
- **Cause:** inherited from the NDISPROT sample. `ndisprotCreateBinding` opened the adapter with
  `FrameTypeArray = {NDIS_ETH_TYPE_802_1X, NDIS_ETH_TYPE_802_1Q}`, and NDIS uses that list to demultiplex
  receives. Broadcast / multicast and the earlier runs still got through, so the filter is not always enforced. That
  made the bug intermittent, and it probably also caused the earlier "no receive after disable/enable".
- **Fix** (`sys/ndisbind.c`): `FrameTypeArray = {0x8892, 0x88CC, 0x88B5, NDIS_ETH_TYPE_802_1Q}`. Build
  hash `BD0DD16AE9FB`, installed in both VMs with `update.cmd`.
- **Verified:** unicast 0x8892 and 0x88B5 from pndevB arrive in pndevA again.
- **Consequence:** D3/D4 results were obtained with the old driver. D4 (CODESYS, physical I219) should be repeated
  with the fixed driver.

## Test 8 and a backend gap: no reconnect after an unbind

- **Test 8** (disable/enable `Ethernet 3` while in DataRun, fixed driver): **no bugcheck**. The AR ends with
  "data-hold timer expired", outputs go to STOP, and the device returns to WaitingForController, as expected.
- **Gap:** after the adapter is enabled again, `pn_device_demo` stays deaf (`DCP: no answer to Identify`) until it is
  restarted. `NdisPort` in `src/pn/l2_port_ndis.cpp` kept the dead handle of the old binding and only slept and
  retried the read.
- **Fix:** when a read cannot be posted, `NdisPort::reopen()` tries the adapter again at most once per second
  (`openAdapter()`: OPEN_DEVICE + SET_FILTER on a new handle). Once that works it cancels the old reads, swaps the
  handle under the send mutex and reposts the reads. `pn_selftest` 112/112. Tools kit `softpn-testkit-20261020`,
  copied to `\\10.0.0.128\D\softpn_ndis_kit\pn`.
- **Verified:** test 8 again; a second controller run reaches DataRun and PASS without restarting
  `pn_device_demo`. `verifier /query`: `load: 3 / unload: 2`, current pool 2 / 790 bytes.

## Final run on the fixed build (2026-10-21)

Driver `BD0DD16AE9FB` + tools `softpn-testkit-20261020`, PROFINET over the new private switch `PN-Private` (adapter
"PN" in both VMs, 10.10.10.1 / 10.10.10.2, see the test-bed notes), Verifier still on, `pn_device_demo` vs
`pn_controller_sim --seconds 1800`, from 00:10 to 00:55:

| | Start 00:10 | End 00:55 |
|---|---|---|
| Pool allocations attempted / succeeded (all special pool) | 69,420 | 294,423 |
| Allocations failed | 0 | 0 |
| Current NonPaged allocations / bytes | 4 / 1,580 | 4 / 1,580 |
| Peak NonPaged allocations / bytes | 111 / 13,434 | 111 / 13,434 |

No bugcheck, no leak (about 225,000 allocations, current count flat). 4 / 1,580 is twice the earlier 2 / 790 because
the driver now has two bindings (Ethernet 3 and PN).

## D4 repeat with the fixed build (2026-10-21)

Physical test PC 10.0.0.193 (Intel I219-LM, test mode) with CODESYS Control Win on PC-PROVE as the PROFINET
controller, 8 ms, kit `softpn-ndis-siemens-kit-20261021`:

- The run before the update still had the old driver (`302C39AF7DD0`, 2026-10-16): DataRun, aborts 0, max TX period
  10.7 ms after 2 min.
- Driver updated with `update.cmd`: installed image `BD0DD16AE9FB` (checked via the service ImagePath in the
  DriverStore), selftest ALL PASS.
- **Result:** `tx 334905 rx 334888, max TX period 11537 us, late 0, ARs 1, aborts 0`. That is about 45 min at 8 ms
  without an abort, and the max TX period equals the old D4 (11.3 ms). **D4 passed with the fixed build.**

## Remaining

- Done 2026-10-21: `verifier /reset` and reboot in pndevA.
- Optional second pass (low-resources simulation, NDIS/WIFI verification), only after a clean standard pass.

## Test-bed notes found on the way

- **Default Switch changes its subnet:** after a restart pndevB got 172.23.9.104 while pndevA kept 192.168.184.38
  (static). DCP (layer 2) still worked, RPC (UDP 34964) went to the NAT gateway and never arrived ("RPC: no response
  to opnum 0"; pktmon showed it). Fix: a private switch only for PROFINET, on PC-PROVE
  `New-VMSwitch -Name "PN-Private" -SwitchType Private` and
  `Add-VMNetworkAdapter -VMName pndevA|pndevB -SwitchName "PN-Private" -Name "PN" -StaticMacAddress 00155D008011|00155D008012`.
  In the VMs the adapter is renamed to "PN" with 10.10.10.1/24 (pndevA) and 10.10.10.2/24 (pndevB), no gateway.
  The firewall needs inbound UDP 34964 (`-Profile Any`). Tests use `--adapter "PN"`; the Default Switch adapters
  stay for the share.
- **pktmon** (built into Windows) shows where frames die: `pktmon filter add -m <MAC>` or `-p <port>`,
  `pktmon start --capture --comp nics`, `pktmon stop`, `pktmon etl2txt PktMon.etl`.

- **No gateway from the Default Switch:** pndevA got no default gateway from DHCP (ping 10.0.0.128 -> "transmit
  failed. General failure"). Fixed with `New-NetRoute -InterfaceAlias "Ethernet 3" -DestinationPrefix 0.0.0.0/0
  -NextHop 192.168.176.1`. The switch IP can change when PC-PROVE reboots.
- **WinDbg without winget:** download `https://aka.ms/windbg/download` (appinstaller), take `MainBundle.Uri`, then
  `Add-AppxPackage` the msixbundle (commands in the Verifier handoff).
- **Stale tools in pndevB:** pndevB still had a `pn_controller_sim.exe` from 2026-10-16 without the WSAStartup fix
  ("RPC: cannot create UDP socket"). Refreshed from the kit (2026-10-17 15:11, 832,000 bytes). Compare sizes after
  copying: the first `copy` did not overwrite the file.
- **`verifier.cmd` seemed to hang:** run `verifier /standard /driver softpn_ndis.sys` directly. It finishes in
  seconds.
