# Phase 9.18: Windows setup and CODESYS bring-up (Gates C and D)

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Connect a real CODESYS PROFINET controller, resolve the `[verify]` items from captures, and run the soak test that decides go / no-go.

## Documentation References
- [CODESYS PROFINET](https://www.codesys.com/products/codesys-fieldbus/profinet.html)
- [Wireshark PROFINET dissectors](https://gitlab.com/wireshark/wireshark/-/tree/master/plugins/epan/profinet)

---

## Step 9.18.1: Windows PC (device side)

### Purpose
Most field problems are NIC settings, not code.

### Procedure
- Dedicated NIC for the machine network, **static IPv4** - the same IP you enter for the device in CODESYS
  (the PoC refuses a DCP Set IP to another address).
- NIC properties: unbind **Microsoft LLDP Protocol Driver**; disable energy-efficient Ethernet / power
  saving and interrupt moderation; Intel "Packet Priority & VLAN": do not strip tags (or disable).
- Firewall: allow inbound **UDP 34964** for the executable. DCP, RT and LLDP are Layer 2 (no firewall).
- Npcap installed (free edition for the lab).
- Run `pn_probe --jitter` on this NIC first (Gate A).

---

## Step 9.18.2: CODESYS controller

### Purpose
Separate hardware: a controller and device on the same NIC do not work through Npcap.

### Procedure
1. CODESYS Control for Linux SL or Raspberry Pi SL (demo mode is enough), Ethernet port cabled to the PC NIC.
2. Tools -> Device Repository -> Install `gsdml/GSDML-V2.43-Univision-SoftPN-*.xml`.
3. Device tree: Ethernet -> PROFINET IO Controller -> add "SoftPN device"; slot 1 "Input N bytes",
   slot 2 "Output N bytes" with N = the demo's sizes (`pn_device_demo` default 32).
4. Device: NameOfStation = `--name`, IP = the PC NIC's IP; update time >= 8 ms.
5. "Scan for devices" -> assign the name; download; start.
6. Device side: `pn_device_demo --adapter "Ethernet 2" --name softpn-device` (or `fb_device_demo --transport profinet`).

---

## Step 9.18.3: Resolve the [verify] items

### Purpose
Values written from the IEC spec as decoded by Wireshark, not yet seen on a real controller.

### Procedure
Capture the startup with Wireshark on the PC NIC and compare (filter `pn_dcp || pn_io || pn_rt || lldp`):

| Item | Where | What to check |
|---|---|---|
| DCP response ServiceType / BlockError values | `pn_defs.hpp` `dcp::` | CODESYS accepts Set responses |
| IOCRProperties RT class (1 or 2) | `pn_ar.cpp` validateConnect | value CODESYS sends |
| Connect error field numbers (ErrorCode2) | `pn_ar.cpp` | only matters for diagnostics |
| ARServerBlock padding / when it is expected | `pn_blocks.cpp` | Connect.res accepted with advanced startup |
| Reads CODESYS issues (e.g. 0xF840) | `pn_ar.cpp` onRead | no read error stops the startup |
| IOPS before ApplicationReady | `pn_cyclic.cpp` | no diagnosis on the controller |
| RTA initial sequence numbers | `pn_rta.hpp` | controller ACKs our ERR / no NACKs |
| RTA abort codes | `pn_defs.hpp` `err::` | controller logs the right reason |
| LLDP Port ID format | `pn_lldp.cpp` | topology view shows the port |
| GSDML attributes (StartupMode, MAUTypeList, ...) | GSDML | Checker + import without warnings |
| Promiscuous needed on the station NIC? | `--promisc 1` | CODESYS scan finds the device without it |

Save small excerpts of the captures under `tests/profinet/captures/` for regression.

---

## Step 9.18.4: Gate C and Gate D

### Purpose
The go / no-go evidence for management.

### Procedure
- **Gate C**: CODESYS completes Connect -> Write -> PrmEnd -> ApplicationReady (device log "AR: Data").
- **Gate D**: device green in CODESYS at 8, 16 and 32 ms; echo visible in the PLC; **1 h soak with
  `aborts 0`** in the demo statistics (and no AR loss in the CODESYS log); PLC STOP/RUN ->
  DataStop/DataRun; cable pull -> abort and automatic reconnect; restart of the demo -> reconnect.
- Record max TX period, late cycles and aborts in `docs/phases/phase-9-profinet.md`.

---

## Verification Checklist

- [ ] Gate C passed with CODESYS
- [ ] Gate D passed: 1 h, 0 aborts at 8 ms (else: at which update time it holds)
- [ ] All `[verify]` items resolved and the docs updated
- [ ] Results page updated

---

## Next Phase
This is the last phase. Update [phase-9-profinet.md](../phase-9-profinet.md) with the results.
