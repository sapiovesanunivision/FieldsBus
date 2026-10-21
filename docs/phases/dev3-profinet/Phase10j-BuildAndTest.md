# Phase 10j: Build and test

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repositories: **FieldsBus** and
> **Dev.3**. Write the results into the tables of [phase-10-dev3-profinet.md](../phase-10-dev3-profinet.md#results).

## Goal
Show that nothing broke (EtherNet/IP, Modbus, existing DLL users), that each byte order is applied on each
transport, and that a real PROFINET controller exchanges the process image with the service on both raw-Ethernet
backends.

## Documentation References
- [Phase 8](../phase-8-dev3.md) 8d / 8e: the loopback procedure with `ProInspectProcessImageService -process`
- [SoftFieldbusPnPLCSetup.md](../../SoftFieldbusPnPLCSetup.md) (10d): PLC side
- [profinet-test-runbook.md](../../profinet-test-runbook.md): network rules for the PROFINET test bed
- Memory rule: **no driver tests on the dev PC (10.0.0.168)**. The softpn_ndis runs are on the test PC 10.0.0.193 or in VMs

---

## Step 10j.1: Builds

### Procedure
- **FieldsBus**: `cmake --build --preset vs2026-release`, `vs2026-analyze` (0 warnings), `pn_selftest` -> **129/129**,
  Linux g++ (WSL Geti) -> 0 warnings.
- **Dev.3** with `NPCAP_SDK` set: `main3.sln` targets `SDK\UvX\SoftFieldbus`, the service, the PLC simulator,
  `UvcIOSoftModBus`, `UnitTestUvcIOSoftModBus`, each in Debug_2026, Release_2026, Debug_2022, Release_2022.
  Debug runs code analysis: no new warnings.
- `UnitTestUvcIOSoftModBus`: 11/11 (the DLL changed under it).
- Without `NPCAP_SDK`: the SoftFieldbus project stops with the Npcap message.
- Installer: `ISCC ProInspectProcessImagesSoftFieldbusService145_x64.iss` compiles.

---

## Step 10j.2: EtherNet/IP and Modbus x byte order (loopback, dev PC)

### Purpose
Same order on both sides: the data must be right. A different order on one side: WORD / DWORD / float must be
visibly wrong while the recipe text stays right. This shows the setting is really applied, on both directions and on
all three transports.

### Procedure
```bat
cd /d D:\Univision\Dev.3\output\v145\Release_2026\bin
start ProInspectProcessImageService145_x64.exe -process
rem one pair per row of the table; ORDER = little | big | big-wordswap
start ProInspectProcessImageSoftFieldbusService145_x64.exe -process modbus-tcp 1502 ORDER
ProInspectProcessImagePlcSim145_x64.exe modbus-tcp --port 1502 --byte-order ORDER --machine 200 --view 1
```
- In the simulator: `d` (recipe code 1234), `w` (recipe), `n` (inputs). Service console `o` / `e`: the same values.
- Machine mode: `acquisitionControl` grows in the process image, view results come back (as in 8e).
- Service side: ProInspect values played on the shared memory, or the `ProInspectProcessImageService` demo keys.
  Simulator `m` (measurements, float) and `p` (packetId) must match.
- Mismatch: service `big`, simulator `little` -> recipeId 1234 shows as 3523477504, packetId jumps by 256.

---

## Step 10j.3: PROFINET with CODESYS

### Procedure
1. **Npcap backend** (dev PC 10.0.0.168, Npcap installed, `L2Backend=npcap`): CODESYS Control Win on 10.0.0.128 with
   the ProInspect GSDML and `ProInspectPn_PLCopen.xml`, update time 16 ms, accepted cycles 10.
   Service: `-process profinet 0 little`, `BindAddress` = 10.0.0.168.
2. **softpn_ndis backend** (test PC 10.0.0.193, driver BD0DD16AE9FB or newer, `L2Backend=ndis`): same CODESYS project
   with the device IP changed. Console as administrator.
3. Each run: PLC `bLayoutOk`, `bPcAlive`, `bByteOrderOk` TRUE; service key `t` "PROFINET RT, state ConnectedRun,
   byte order little"; key `o` recipeId 1234; ProInspect (or the shared memory) sees `acquisitionControl` change
   every 2 s; PLC sees `packetId`, view results and measurements from ProInspect.
4. STOP/RUN on the PLC and a service restart: reconnects without action (as in D4: 1-4 s).
5. Soak: 30 min, event log without aborts; diagnostic shared memory cycle statistics.
6. Byte-order mismatch: restart the service with `big`. `bByteOrderOk` goes FALSE, recipeId 3523477504.

---

## Step 10j.4: PROFINET with Siemens

### Procedure
1. S7-1500 (colleague's test bed, see `docs/HANDOFF-softpn-ndis-siemens.md`): TIA project with the ProInspect GSDML and
   `ProInspectPn.scl`; service with `ByteOrder=big` (default), backend as available.
2. Check `bLayoutOk` (`iRetRead` / `iRetWrite` = 0), `bPcAlive`, `bByteOrderOk`; recipeId 1234 on the PC;
   a REAL from ProInspect (for example measurement 0 = 1.5) reads 1.5 in the instance DB.
3. If `DPRD_DAT` returns an error for 256 bytes on the CPU, note the code and switch to the I/Q tag fallback of 10d.

---

## Step 10j.5: Record and close

### Procedure
- Fill the results tables in [phase-10-dev3-profinet.md](../phase-10-dev3-profinet.md#results) with the Dev.3 commit
  hashes, and the results table of `SoftFieldbusPnPLCSetup.md`.
- Close the Dev.3 item in [phase-9-profinet.md](../phase-9-profinet.md) (open items).
- Mark these step documents "verified" where the code was built as written, and fix the blocks that differ.

---

## Verification Checklist

- [ ] All builds of 10j.1 green; `pn_selftest` 129/129; `UnitTestUvcIOSoftModBus` 11/11
- [ ] 9 regression runs (3 transports x 3 byte orders) + mismatch detected on each transport
- [ ] CODESYS: ConnectedRun with Npcap and with softpn_ndis; data both ways; STOP/RUN; 30 min soak
- [ ] Siemens: ConnectedRun with `big`; INT / DWORD / REAL right without SWAP
- [ ] Results and commit hashes recorded

---

## Next Phase
This is the last sub-phase. Back to the overview: [phase-10-dev3-profinet.md](../phase-10-dev3-profinet.md).
