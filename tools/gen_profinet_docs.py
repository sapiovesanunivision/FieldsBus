"""Generates docs/phases/profinet/*.md (spec-to-code format) from the verified sources on
feature/profinet-poc. Run from the repo root: python tools/gen_profinet_docs.py
Code blocks are read from the repo files, so the docs always match the compiled code."""
import os
import textwrap

OUT = 'docs/phases/profinet'
LANG = {'.hpp': 'cpp', '.cpp': 'cpp', '.h': 'cpp', '.txt': 'cmake', '.xml': 'xml', '.cs': 'csharp', '.md': 'markdown'}

WIRESHARK = '[Wireshark PROFINET dissectors](https://gitlab.com/wireshark/wireshark/-/tree/master/plugins/epan/profinet)'
SPEC = '[Phase 9 spec](../../profinet-spec.md)'


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read().replace('\r\n', '\n').rstrip('\n')


def lang_of(path):
    if path.endswith('CMakeLists.txt'):
        return 'cmake'
    return LANG.get(os.path.splitext(path)[1], '')


def fence(code, lang):
    # A file that itself contains ``` would break the fence; none of ours does, but be safe.
    tick = '````' if '```' in code else '```'
    return f'{tick}{lang}\n{code}\n{tick}'


def F(path, purpose, concepts, title=None, note=None):
    """Step: the complete file from the repo."""
    return dict(kind='file', path=path, purpose=purpose, concepts=concepts, title=title, note=note)


def S(path, title, purpose, code, concepts, lang=None, instruction=None):
    """Step: a snippet / edit instruction."""
    return dict(kind='snippet', path=path, title=title, purpose=purpose, code=textwrap.dedent(code).strip('\n'),
                concepts=concepts, lang=lang or lang_of(path), instruction=instruction)


def T(title, purpose, body, concepts=None):
    """Step: text only (procedures, commands)."""
    return dict(kind='text', title=title, purpose=purpose, body=textwrap.dedent(body).strip('\n'), concepts=concepts or [])


PHASES = []


def phase(num, slug, title, goal, refs, steps, verify, architecture=None, results=None):
    PHASES.append(dict(num=num, slug=slug, title=title, goal=goal, refs=refs, steps=steps, verify=verify,
                       architecture=architecture, results=results))


# =====================================================================================================
# Milestone A - L2 access and timing
# =====================================================================================================

phase('9.01', 'BuildScaffolding', 'Build scaffolding, constants and the L2 interface',
      'Add the `softpn` library to the build (only when the Npcap SDK is available on Windows), the '
      'big-endian helpers PROFINET needs, the protocol constants, the `L2Port` interface with its frame '
      'helpers and an in-memory "cable" used by the offline tests.',
      [SPEC, WIRESHARK, '[Npcap SDK](https://npcap.com/#download)'],
      [
          F('include/softeip/bytes.hpp',
            'PROFINET blocks are big-endian, CIP is little-endian. The existing reader/writer gets the missing '
            'big-endian 32-bit accessors, in-place patching (block lengths are written last) and `pos()`/`seek()`. '
            'All additions are new members: existing EtherNet/IP and Modbus code is unchanged.',
            ['**`patchU16be`**: write a placeholder length, append the block body, then patch the real length.',
             '**`ParseError`**: every bounds violation throws; protocol handlers catch it and answer with an error '
             'instead of crashing on a malformed frame.'],
            title='Big-endian helpers in the shared byte reader/writer'),
          F('include/softeip/export.hpp',
            'Adds `SOFTPN_API` so `softpn::Device` can be exported from `SoftFieldbus.dll` exactly like the '
            'EtherNet/IP and Modbus classes (`SOFTFIELDBUS_SHARED` turns all of them on).',
            ['**Same pattern as `SOFTFB_API`**: empty for static libraries, dllexport/dllimport for the DLL build.'],
            title='Export macro for the PROFINET classes'),
          F('src/pn/pn_defs.hpp',
            'One place for every protocol constant: EtherTypes, FrameIDs, DCP options, RPC packet types and '
            'opnums, PNIO block types, data status bits, RTA PDU types and the PNIOStatus error codes. Values '
            'marked `[verify]` must be confirmed against a CODESYS capture during bring-up (Phase 9.18).',
            ['**FrameID ranges**: 0x8000-0xBFFF cyclic RT_CLASS_1, 0xFC01/0xFE01 alarms, 0xFEFC-0xFEFF DCP.',
             '**DataStatus 0x35**: primary + data valid + provider RUN + station OK, the value a healthy provider sends.',
             '**PNIOStatus**: 4 bytes ErrorCode / ErrorDecode / ErrorCode1 / ErrorCode2, e.g. `DB 81 02 0B` = Connect, '
             'PNIO, IOCR block, field 11 (reduction ratio).']),
          F('include/softpn/l2_port.hpp',
            'The stack never touches Npcap or sockets directly: it sends and receives Ethernet frames through '
            '`L2Port`. Backends: Npcap (Windows), AF_PACKET (Linux), memory (tests). A future NDISPROT-based '
            'driver (no Npcap licence) would be one more backend.',
            ['**`receive()` with timeout + `interrupt()`**: lets `stop()` end the RX thread at once.',
             '**`promiscuous = false`** by default: on a Hyper-V adapter promiscuous mode stopped reception after '
             'a few seconds, and DCP multicast arrived without it (see Phase 9.05 results).',
             '**`extraEtherType`**: `pn_probe` also receives its 0x88B5 test frames.']),
          F('src/pn/pn_frame.hpp',
            'Ethernet II framing shared by DCP, LLDP, cyclic data and alarms: optional 802.1Q tag, padding to '
            '60 bytes, FrameID access.',
            ['**VLAN tolerance**: received frames may or may not carry a tag (some NIC drivers strip it), so '
             '`parseEthHeader` handles both and reports `payloadOffset`.']),
          F('src/pn/pn_frame.cpp',
            'MAC/IP text conversion (adapter selection by MAC or IP) and the frame helpers.',
            ['**Host byte order**: IPv4 addresses are kept as host-order `uint32_t` everywhere in softpn.']),
          F('src/pn/l2_port_memory.cpp',
            'Two `L2Port`s joined by an in-memory cable. `pn_selftest` (Phase 9.15) runs the real device '
            'against the controller simulator over it - no NIC, no Npcap, no admin rights.',
            ['**Bounded queue**: if nobody reads, `send()` fails like a full NIC buffer instead of growing memory.']),
          S('CMakeLists.txt', 'Add the softpn library to CMake',
            'Insert this block **before** the `# ---- Everything in one DLL` section. On Windows the library is '
            'built only when `SOFTPN_NPCAP_SDK` (default: the `NPCAP_SDK` environment variable) points to the '
            'Npcap SDK, so existing builds and Dev.3 are not affected. Later phases append files to the two lists.',
            '''
            # ---- PROFINET RT device (softpn): raw Ethernet via Npcap (Windows) or AF_PACKET (Linux) ----
            set(SOFTPN_NPCAP_SDK "$ENV{NPCAP_SDK}" CACHE PATH "Npcap SDK folder (Include/, Lib/x64/); empty = no PROFINET on Windows")
            set(SOFTPN_ENABLED OFF)
            if(WIN32)
                if(SOFTPN_NPCAP_SDK AND EXISTS "${SOFTPN_NPCAP_SDK}/Include/pcap.h")
                    set(SOFTPN_ENABLED ON)
                else()
                    message(STATUS "softpn (PROFINET) skipped: set SOFTPN_NPCAP_SDK to the Npcap SDK folder to build it")
                endif()
            elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                set(SOFTPN_ENABLED ON)
            endif()

            if(SOFTPN_ENABLED)
                set(SOFTPN_SOURCES
                    src/pn/pn_frame.cpp
                    src/pn/l2_port_memory.cpp
                )
                set(SOFTPN_HEADERS
                    include/softpn/l2_port.hpp
                    src/pn/pn_defs.hpp
                    src/pn/pn_frame.hpp
                )
                add_library(softpn STATIC ${SOFTPN_SOURCES} ${SOFTPN_HEADERS})
                source_group("Header Files" FILES ${SOFTPN_HEADERS})
                target_include_directories(softpn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/pn)
                target_link_libraries(softpn PUBLIC softeip)
                softeip_warnings(softpn)
                set_target_properties(softpn PROPERTIES FOLDER "native")
            endif()
            ''',
            ['**`SOFTPN_ENABLED`**: one switch that later phases (tools, FieldbusDevice, SoftFieldbus.dll) test.',
             '**`target_link_libraries(softpn PUBLIC softeip)`**: reuses the byte helpers, timers and socket layer.']),
          T('Install the Npcap SDK', 'Needed to compile the Windows backend (Phase 9.02). The runtime is needed to run it.',
            '''
            1. Download the **Npcap SDK** (e.g. 1.16) from https://npcap.com/#download and unzip it, e.g. to `D:\\Software\\npcap-sdk-1.16`.
            2. Set the user environment variable `NPCAP_SDK` to that folder and restart Visual Studio / VS Code.
            3. Install the **Npcap runtime** (free edition, development PCs only - at most 5 systems; shipping on
               customer machines needs Npcap OEM). Installer options: leave "restrict to administrators" unchecked;
               "WinPcap API-compatible mode" is optional (softpn loads `wpcap.dll` from `System32\\Npcap` either way).
            4. Regenerate: `cmake --preset vs2026` (or `generate_vs2026.bat open`).
            '''),
      ],
      ['`cmake --preset vs2026` prints no "softpn skipped" message when `NPCAP_SDK` is set, and prints it when it is not',
       'The `softpn` project builds with 0 warnings (`/W4`); the rest of the solution is unchanged',
       'Linux: `cmake --preset linux && cmake --build --preset linux` builds `softpn` with 0 warnings',
       'Existing tests still pass: `eip_scanner_sim` against `eip_adapter_demo`, `mb_client_test` against `mb_server_demo`'],
      architecture='''
      PROFINET protocol code (DCP, RPC, AR, cyclic, RTA)
                    |
                    v
             +-------------+
             |   L2Port    |  send() / receive(timeout) / interrupt() / info()
             +-------------+
              |     |     |
         Npcap   AF_PACKET  memory cable
        (9.02)    (9.03)    (tests, this phase)
      ''')

phase('9.02', 'L2PortNpcap', 'Npcap backend (Windows)',
      'Raw Ethernet on Windows through Npcap: adapter enumeration with MAC/IP, a BPF filter that only lets '
      'PROFINET and LLDP frames through, event-driven receive and a separate transmit handle.',
      ['[Npcap guide](https://npcap.com/guide/)', '[pcap filter syntax](https://www.tcpdump.org/manpages/pcap-filter.7.html)',
       '[GetAdaptersAddresses](https://learn.microsoft.com/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersaddresses)'],
      [
          F('src/pn/l2_port_npcap.cpp',
            'Implements `listAdapters()` and `openL2Port()` with Npcap. The adapter can be selected by the Npcap '
            'name, the Windows friendly name ("Ethernet 2"), the MAC or the IPv4 address.',
            ['**Delay-loaded `wpcap.dll`**: loaded by full path from `System32\\Npcap` before the first call, so a '
             'PC without Npcap gets a clear error instead of a crash, and old `wpcap.dll` copies elsewhere are ignored.',
             '**Two pcap handles**: a `pcap_t` must not be used by two threads; RX waits on `pcap_getevent()`, TX has '
             'a filter that captures nothing (`less 1`).',
             '**Filter order matters**: libpcap\'s `vlan` keyword shifts the offsets of every term after it, so it is '
             'the last term (`not ether src <own MAC> and (... or (vlan and ether proto 0x8892))`). With `vlan` in '
             'the middle, any EtherType test after it silently never matched - found with the Phase 9.05 probe.',
             '**Own frames**: Npcap loops transmitted frames back to other handles; the filter drops them.']),
          S('CMakeLists.txt', 'Compile and link the Npcap backend',
            'Inside `if(SOFTPN_ENABLED)`: append the platform backend after `set(SOFTPN_SOURCES ...)`, and add the '
            'Windows link settings after `softeip_warnings(softpn)`.',
            '''
                if(WIN32)
                    list(APPEND SOFTPN_SOURCES src/pn/l2_port_npcap.cpp)
                else()
                    list(APPEND SOFTPN_SOURCES src/pn/l2_port_linux.cpp)
                endif()

                # ... add_library(softpn ...) / softeip_warnings(softpn) as before ...

                if(WIN32)
                    target_include_directories(softpn PRIVATE "${SOFTPN_NPCAP_SDK}/Include")
                    target_link_libraries(softpn PUBLIC "${SOFTPN_NPCAP_SDK}/Lib/x64/wpcap.lib" iphlpapi delayimp)
                    # wpcap.dll is loaded on first use from System32\\Npcap: no Npcap = error message, not a crash.
                    target_link_options(softpn INTERFACE "/DELAYLOAD:wpcap.dll")
                endif()
            ''',
            ['**`INTERFACE` link option**: every executable that links `softpn` gets `/DELAYLOAD:wpcap.dll`.',
             '**Linux line already here**: Phase 9.03 adds `l2_port_linux.cpp`; until then build on Windows only.']),
      ],
      ['`softpn` builds with 0 warnings on Windows',
       'After Phase 9.05, `pn_probe --list` shows every adapter with MAC, IPv4 and the `\\Device\\NPF_{GUID}` name',
       'On a PC without Npcap, `pn_probe --list` prints "Npcap is not installed ..." instead of crashing'])

phase('9.03', 'L2PortLinux', 'AF_PACKET backend (Linux)',
      'The same `L2Port` on Linux, for development, CI and the veth/netns tests. Needs `CAP_NET_RAW`.',
      ['[packet(7)](https://man7.org/linux/man-pages/man7/packet.7.html)', '[getifaddrs(3)](https://man7.org/linux/man-pages/man3/getifaddrs.3.html)'],
      [
          F('src/pn/l2_port_linux.cpp',
            'Raw socket bound to one interface, joined to the DCP and LLDP multicast groups, with an eventfd to '
            'interrupt `poll()`. Frames are filtered in user space (PROFINET, LLDP, the optional extra EtherType).',
            ['**`PACKET_ADD_MEMBERSHIP`**: Linux can join the DCP multicast group, so promiscuous mode is not needed.',
             '**`PACKET_OUTGOING`**: our own transmissions are skipped.',
             '**Rights**: run as root or `sudo setcap cap_net_raw+ep ./pn_device_demo`.']),
      ],
      ['`cmake --build --preset linux` builds `softpn` with 0 warnings (`-Wall -Wextra -Wpedantic`)',
       'After Phase 9.05: `sudo ./pn_probe --list` shows the interfaces with MAC and IPv4'])

phase('9.04', 'Dcp', 'DCP: discovery and naming',
      'Implement DCP, the protocol a controller (and CODESYS "Scan for devices" or Siemens Proneta) uses to find '
      'devices, assign the NameOfStation and IP suite, and flash the identification LED. The NameOfStation set '
      'by the controller is persisted.',
      [WIRESHARK + ' (`packet-pn-dcp.c`)', '[PI: PROFINET system description](https://www.profibus.com/download/profinet-system-description)'],
      [
          F('src/pn/pn_station_store.hpp', 'The NameOfStation assigned by the controller must survive a restart.',
            ['**INI file**: human-readable, `[station] name=... ipSet=...`.']),
          F('src/pn/pn_station_store.cpp', 'Load and atomic save (write a temp file, then rename over the old one).',
            ['**Atomic replace**: a power loss during the write never leaves a half-written file.']),
          F('src/pn/pn_dcp.hpp',
            'DCP codec (PDU <-> bytes) plus the device-side responder. The responder talks to the rest of the '
            'device only through hooks (send, log, signal, persist, in-operation), which keeps it testable.',
            ['**Block payload**: responses carry a 2-byte BlockInfo, Set requests a 2-byte BlockQualifier, Get '
             'requests only bare option/suboption pairs (`bare = true`).',
             '**Thread model**: `onFrame()` runs on the RX thread, `poll()` on the cyclic thread sends delayed answers.']),
          F('src/pn/pn_dcp.cpp',
            'Identify with name/ID filters and ResponseDelay, Get, Set (NameOfStation, IP, Signal, factory reset), '
            'NameOfStation validation.',
            ['**IP rule (PoC)**: Windows owns the NIC address. A Set IP equal to the NIC\'s static IP is accepted; '
             'anything else is refused (BlockError 5) with a log telling you to configure the PC\'s IP in the controller.',
             '**Persist before answering**: the controller may power-cycle the device right after the Set response.',
             '**ResponseDelay**: answers to an Identify-All are spread over `ResponseDelay x 10 ms` (derived from the MAC) '
             'so a network of devices does not answer at once.',
             '**Name rules**: lowercase DNS labels, max 240 characters, no `port-xyz`, not an IP address.']),
          S('CMakeLists.txt', 'Add the DCP files',
            'Append to the lists inside `if(SOFTPN_ENABLED)`.',
            '''
                set(SOFTPN_SOURCES
                    ...
                    src/pn/pn_station_store.cpp
                    src/pn/pn_dcp.cpp
                )
                set(SOFTPN_HEADERS
                    ...
                    src/pn/pn_station_store.hpp
                    src/pn/pn_dcp.hpp
                )
            ''', ['**Order**: the final list is shown in Phase 9.16.']),
      ],
      ['`softpn` builds with 0 warnings',
       'The DCP responder is exercised end-to-end from Phase 9.13 on (`pn_probe --identify`, CODESYS scan) and offline in Phase 9.15',
       '`pn_selftest` (Phase 9.15) "DCP codec" and "NameOfStation rules" sections pass'])

phase('9.05', 'ProbeAndJitterGate', 'pn_probe and the timing gate (Gate A)',
      'A diagnostic tool for Layer 2: list adapters, measure how regularly Windows can send frames (the decisive '
      'question for a software PROFINET device), measure arrival jitter on a second machine, and run DCP '
      'Identify / Set name / Blink like a mini Proneta.',
      ['[CreateWaitableTimerExW](https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)',
       '[timeBeginPeriod and power throttling](https://learn.microsoft.com/windows/win32/api/timeapi/nf-timeapi-timebeginperiod)'],
      [
          F('include/softeip/periodic_timer.hpp',
            'Adds `DeadlineTimer` next to the existing `PeriodicTimer`. Measurements on this PC showed that a '
            'periodic 1 ms waitable timer actually ticks about every 1.5 ms; a one-shot high-resolution timer set '
            'to the exact due time wakes much closer to it.',
            ['**`waitUntil(steady_clock::time_point)`**: the cyclic thread computes absolute due times '
             '(`next += period`), so a late wake-up delays one frame but never shifts the schedule.',
             '**Additive**: `PeriodicTimer` (used by EtherNet/IP) is unchanged.'],
            title='DeadlineTimer: wake up at an absolute time'),
          F('tools/pn_probe.cpp',
            'The tool. `--jitter` uses exactly the device\'s scheduling (Phase 9.13), so its numbers predict the '
            'device\'s TX timing. 0x88B5 (IEEE local experimental) test frames are ignored by PROFINET devices.',
            ['**`--dst MAC`**: physical switches flood frames to an unknown MAC, Hyper-V switches drop them - send '
             'to the receiver\'s MAC when measuring through a virtual switch.',
             '**`send()` duration**: how long `pcap_sendpacket` blocks; outliers here add to the jitter.',
             '**Histogram of |deviation|**: what matters is the tail, because the PLC aborts after DataHoldFactor '
             'missed periods (typically 3 x update time).']),
          S('CMakeLists.txt', 'Build pn_probe',
            'Inside `if(SOFTPN_ENABLED)`, after the library:',
            '''
                add_executable(pn_probe tools/pn_probe.cpp)
                target_include_directories(pn_probe PRIVATE src/pn)
                target_link_libraries(pn_probe PRIVATE softpn)
                softeip_warnings(pn_probe)
                set_target_properties(pn_probe PROPERTIES FOLDER "tools" VS_DEBUGGER_COMMAND_ARGUMENTS "--list")
            ''', ['**`src/pn` include path**: the probe uses internal headers (DCP codec, frame helpers).']),
          T('Run Gate A', 'Decide early whether the PC can hold the update time before investing in the rest.',
            '''
            ```bat
            pn_probe --list
            pn_probe --adapter "Ethernet 2" --jitter --period-ms 8 --seconds 60
            pn_probe --adapter "Ethernet 2" --jitter --period-ms 4 --seconds 60
            ```
            Run it on the **dedicated machine NIC** with the PC under its normal load (vision application running).
            Optional: on a second PC cabled to it, `pn_probe --adapter X --rx-jitter --seconds 60` while the first
            runs `--jitter --dst <MAC of X>`, to see the jitter as the PLC will see it.

            **Gate A passes when** at 8 ms: p99 period error < 0.5 ms and max < 2 ms, and no gap reaches
            3 x 8 = 24 ms (the PLC's default watchdog).
            '''),
      ],
      ['`pn_probe --list` lists the adapters with MAC / IPv4 / Npcap name',
       'Gate A numbers recorded in `docs/phases/phase-9-profinet.md` for the target PC and NIC',
       '`pn_probe --identify` (after Phase 9.13) finds the softpn device and any other PROFINET device on the wire'],
      results='''
      Reference measurement (development laptop, Intel I219-LM idle; test frames on the internal Hyper-V
      "Default Switch" adapter, so no LAN traffic; desktop with VS Code running):

      | Scheduling | Period | avg | p99 | max | within 0.5 ms |
      |---|---|---|---|---|---|
      | periodic timer, 1 frame per tick | 8 ms | 8.155 ms (drifts 2 %) | 8.55 ms | 8.78 ms | 94 % |
      | periodic timer, 1 frame per tick | 1 ms | 1.47 ms (only ~680 frames/s) | 1.89 ms | 8.6 ms | 26 % |
      | **DeadlineTimer (device scheduling)** | 8 ms | 8.000 ms | 8.61 ms | 11.5 ms | 97 % |
      | **DeadlineTimer** | 4 ms | 4.000 ms | 4.47 ms | 5.6 ms | 98 % |
      | **DeadlineTimer** | 1 ms | 1.005 ms (19 900 / 20 000 frames) | 1.58 ms | 2.6 ms | 98 % |

      `send()` (pcap_sendpacket): p50 ~150 us, p99 ~400-550 us, rare outliers of several ms on the virtual adapter.
      Repeat on the real station NIC - this table is a reference, not the gate result.
      ''')

# =====================================================================================================
# Milestone B/C - LLDP, RPC, blocks, model
# =====================================================================================================

phase('9.06', 'Lldp', 'LLDP transmitter',
      'Send LLDP every 5 s with the PROFINET TLVs, so controllers and engineering tools see the device in their '
      'topology view.',
      ['[IEEE 802.1AB (LLDP)](https://en.wikipedia.org/wiki/Link_Layer_Discovery_Protocol)', WIRESHARK + ' (`packet-lldp.c`)'],
      [
          F('src/pn/pn_lldp.hpp', 'One function: build the LLDP frame from the current identity.',
            ['**Rebuilt on change**: the device rebuilds the frame when the NameOfStation changes (Chassis ID).']),
          F('src/pn/pn_lldp.cpp',
            'Chassis ID (NameOfStation, or MAC while unnamed), Port ID `port-001`, TTL 20 s, PROFINET delay / port '
            'status / chassis MAC TLVs, IEEE 802.3 MAC/PHY TLV.',
            ['**TLV header**: 7-bit type + 9-bit length - built with an unsigned shift (`/analyze` flagged the first '
             'version, which shifted a 16-bit value).',
             '**Windows LLDP driver**: unbind "Microsoft LLDP Protocol Driver" from the machine NIC, or two LLDP '
             'senders share one port.']),
          S('CMakeLists.txt', 'Add the LLDP files', 'Append to the lists:',
            '''
                    src/pn/pn_lldp.cpp      # SOFTPN_SOURCES
                    src/pn/pn_lldp.hpp      # SOFTPN_HEADERS
            ''', ['**Transmission** happens in the device\'s cyclic thread (Phase 9.13).']),
      ],
      ['`softpn` builds with 0 warnings',
       'After Phase 9.13: Wireshark shows an LLDP frame every 5 s from the device with no "malformed" markers',
       '`[verify]` with CODESYS: the Port ID format it expects (`port-001`)'])

phase('9.07', 'RpcCodecAndEndpoint', 'DCE/RPC (connectionless) over UDP 34964',
      'PROFINET context management (Connect, Write, Read, Control, Release) runs as DCE/RPC calls over UDP. '
      'Implement UUIDs, the 80-byte RPC header in both byte orders, the NDR argument wrapper and a UDP endpoint '
      'that serves requests and sends its own (ApplicationReady).',
      ['[DCE 1.1 RPC, chapter 12 (CL protocol)](https://pubs.opengroup.org/onlinepubs/9629399/chap12.htm)',
       WIRESHARK + ' (`packet-dcerpc.c`, `packet-dcerpc-pn-io.c`)'],
      [
          F('src/pn/pn_uuid.hpp',
            'UUID type with byte-order aware encoding, random UUIDs for RPC activities, and the PNIO interface '
            'and object UUIDs.',
            ['**Two byte orders**: inside PNIO blocks a UUID is big-endian; in the RPC header Data1-3 follow the '
             'packet\'s drep (usually little-endian).',
             '**Device object UUID**: `DEA00000-6C97-11D1-8271-<instance><deviceId><vendorId>`.']),
          F('src/pn/pn_rpc.hpp', 'Header/NDR codecs, `PnioStatus`, and the `RpcEndpoint` API.',
            ['**`RequestHandler` returns false** to send a "reject" (unknown interface/operation).',
             '**`UdpAddress`**: host-order IP + port, keeps socket types out of the header.']),
          F('src/pn/pn_rpc.cpp',
            'Codecs and the endpoint: fragment reassembly, fragmentation of large responses, response cache per '
            'activity (retransmitted requests and pings get the same answer), fack, reject.',
            ['**drep**: parse both byte orders, always answer little-endian (allowed by DCE).',
             '**Idempotent retransmission**: the controller resends a request with the same activity + seqnum if '
             'the answer is lost; the cached packets are resent, the handler is not called twice.',
             '**`disableUdpConnReset`**: on Windows an ICMP "port unreachable" would otherwise break the socket.']),
          S('CMakeLists.txt', 'Add the RPC files', 'Append to the lists:',
            '''
                    src/pn/pn_rpc.cpp                       # SOFTPN_SOURCES
                    src/pn/pn_uuid.hpp src/pn/pn_rpc.hpp    # SOFTPN_HEADERS
            ''', ['**Firewall**: the device needs inbound UDP 34964 (Phase 9.18).']),
      ],
      ['`softpn` builds with 0 warnings (MSVC `/W4`, g++)',
       '`pn_selftest` (Phase 9.15) "RPC header / NDR" passes, including the big-endian header variant'])

phase('9.08', 'PnioBlocks', 'PNIO blocks (Connect, Control, Read/Write, I&M0)',
      'Codec for every block the device receives and sends - in both directions, so the controller simulator '
      '(Phase 9.14) reuses the same code.',
      [WIRESHARK + ' (`packet-dcerpc-pn-io.c`: block layouts and field names)'],
      [
          F('src/pn/pn_blocks.hpp',
            'Structures for ARBlockReq/Res, IOCRBlockReq/Res, AlarmCRBlockReq/Res, ExpectedSubmoduleBlockReq, '
            'ModuleDiffBlock, ARServerBlock, the control blocks, record headers and I&M0.',
            ['**`BlockWriter`**: writes the header, patches BlockLength (bytes after the length field) on finish.',
             '**Record data is not a block**: in a Write request and a Read response the data follows the 60-byte '
             'header directly.',
             '**SubmoduleState**: format bit + IdentInfo (OK / substitute / wrong / no submodule).']),
          F('src/pn/pn_blocks.cpp',
            'Parse and build functions. `parseConnectRequest` collects all Connect blocks and turns a malformed '
            'one into the PNIOStatus to return.',
            ['**Unknown blocks are ignored** (ARVendorBlockReq, PrmServerBlock...): counted, not fatal.',
             '**ExpectedSubmodule DataDescription count**: one for input or output, two for input+output, one '
             '(zero-length input) for "no IO".']),
          S('CMakeLists.txt', 'Add the block codec', 'Append to the lists:',
            '''
                    src/pn/pn_blocks.cpp      # SOFTPN_SOURCES
                    src/pn/pn_blocks.hpp      # SOFTPN_HEADERS
            ''', ['**Next**: the device model compares the expected submodules with the real ones.']),
      ],
      ['`softpn` builds with 0 warnings', '`pn_selftest` "PNIO blocks" passes (round trips, record header size 64, I&M0 size 60)'])

phase('9.09', 'ModelAndGsdml', 'Device model and GSDML',
      'Define the slot layout, the module variants and their ident numbers once in code, write the GSDML the '
      'controller imports, and compare the controller\'s expected configuration with the real one.',
      ['[PI GSDML specification and GSDML Checker](https://www.profibus.com/download/gsdml-specification-for-profinet)'],
      [
          F('src/pn/pn_model.hpp',
            'Slot 0: DAP, interface (0x8000), port (0x8001). Slot 1: input module N bytes (PC -> PLC). Slot 2: '
            'output module N bytes (PLC -> PC). N = 16/32/64/128/256.',
            ['**Ident numbers**: inputs 0x10-0x14, outputs 0x20-0x24, DAP 0x1 - must match the GSDML.',
             '**`ConfigCheck.proper`**: only matching submodules get IOPS GOOD in the cyclic data.']),
          F('src/pn/pn_model.cpp', 'The variant table and `compareExpected()`, which produces the ModuleDiffBlock.',
            ['**Wrong length = wrong submodule**: a controller expecting 32 bytes where the device has 64 gets a '
             'ModuleDiff entry, not silently truncated data.']),
          F('gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml',
            'The device description for CODESYS / TIA. One DAP, ten modules (5 input + 5 output sizes), '
            'RT_CLASS_1, send clock 1 ms, reduction ratio 8..512 (update time >= 8 ms).',
            ['**VendorID 0xFFFF is a placeholder**: replace it (and `DeviceConfig::vendorId`) with the PI vendor ID '
             'before any use outside the lab.',
             '**Validate** with the PI GSDML Checker after every change and note its warnings in the results page.',
             '**`StartupMode="Advanced;Legacy"`**, `ResetToFactoryModes`, `MAUTypeList`: `[verify]` what CODESYS accepts.'],
            note='The file was generated from a small script so the ten module entries stay consistent; edit it by hand from now on.'),
          S('CMakeLists.txt', 'Add the model', 'Append to the lists:',
            '''
                    src/pn/pn_model.cpp      # SOFTPN_SOURCES
                    src/pn/pn_model.hpp      # SOFTPN_HEADERS
            ''', ['**Self-check**: `pn_selftest` greps the GSDML for every ident number of the table.']),
      ],
      ['`softpn` builds with 0 warnings', 'The GSDML is well-formed XML (opens in a browser / `xmllint`)',
       'PI GSDML Checker run; warnings noted', 'CODESYS: Tools -> Device Repository -> Install imports the file',
       '`pn_selftest` "GSDML ident numbers" passes'])

# =====================================================================================================
# Milestone D - cyclic, alarms, AR, device
# =====================================================================================================

phase('9.10', 'CyclicEngine', 'Cyclic RT_CLASS_1 data exchange',
      'The provider sends the input CR (device -> controller) on time; the consumer parses the output CR and a '
      'data-hold watchdog ends the AR when the controller\'s frames stop.',
      [WIRESHARK + ' (`packet-pn-rt.c`)'],
      [
          F('src/pn/pn_cyclic.hpp',
            'The C_SDU layout comes from the controller (IOCRBlockReq frame offsets), so `CyclicSetup` stores '
            'positions, not a fixed format.',
            ['**Frame**: `FrameID | C_SDU | CycleCounter | DataStatus | TransferStatus`.',
             '**IOPS / IOCS**: one status byte per submodule and direction; 0x80 = GOOD.',
             '**`nextDue()`**: the cyclic thread sleeps exactly until the next frame is due (Phase 9.05 results).']),
          F('src/pn/pn_cyclic.cpp',
            'Frame building (data, IOPS, IOCS, cycle counter in 31.25 us units), period statistics, consumer '
            'evaluation (provider RUN, data valid, IOPS) and the watchdog.',
            ['**No burst after a stall**: if the thread falls behind, the schedule is resynchronised instead of '
             'sending several frames back to back.',
             '**Watchdog grace**: armed at ApplicationReady (or the first frame), then `DataHoldFactor x period`.',
             '**Callbacks without locks**: `onOutputs` / `onDataHoldExpired` are called after the mutex is released.']),
          S('CMakeLists.txt', 'Add the cyclic engine', 'Append to the lists:',
            '''
                    src/pn/pn_cyclic.cpp      # SOFTPN_SOURCES
                    src/pn/pn_cyclic.hpp      # SOFTPN_HEADERS
            ''', ['**Thread**: driven by the device\'s cyclic thread (Phase 9.13).']),
      ],
      ['`softpn` builds with 0 warnings',
       'Phase 9.15: cyclic echo > 60 frames/s at 8 ms, device max TX period < 9 ms over the memory cable'])

phase('9.11', 'RtaAlarms', 'Alarm transport (RTA)',
      'Keep the AlarmCR healthy: acknowledge alarm DATA from the controller, end the AR when the controller sends '
      'an ERR, and notify the controller with an ERR when the device aborts.',
      [WIRESHARK + ' (`packet-pn-rtc-one.c`: RTA PDUs)'],
      [
          F('src/pn/pn_rta.hpp', 'Two channels (high 0xFC01, low 0xFE01) with their own sequence numbers.',
            ['**PoC scope**: process/diagnosis alarms from the device (DATA) are not sent yet.',
             '**`[verify]`**: the initial sequence numbers, against a CODESYS capture.']),
          F('src/pn/pn_rta.cpp', 'PDU build/parse, ACK for DATA, ERR handling.',
            ['**Endpoints**: AlarmDstEndpoint = the controller\'s LocalAlarmReference, AlarmSrcEndpoint = ours.',
             '**ERR var part** = the PNIOStatus, e.g. `CF 81 FD 05` (consumer data-hold expired).']),
          S('CMakeLists.txt', 'Add RTA', 'Append to the lists:',
            '''
                    src/pn/pn_rta.cpp      # SOFTPN_SOURCES
                    src/pn/pn_rta.hpp      # SOFTPN_HEADERS
            ''', ['**Used by** the AR state machine (abort) and the RX thread.']),
      ],
      ['`softpn` builds with 0 warnings', 'Phase 9.15: after a watchdog abort the controller simulator receives an RTA ERR'])

phase('9.12', 'ArStateMachine', 'AR state machine',
      'The "connection": validate a Connect, build the response, accept parameter records, PrmEnd, send '
      'ApplicationReady, enter data exchange, and end the AR on Release, watchdog, ERR or timeout.',
      [WIRESHARK + ' (`packet-dcerpc-pn-io.c`: PNIO-CM)', SPEC + ' section 4.9'],
      [
          F('src/pn/pn_ar.hpp', 'States, configuration and the hooks to the cyclic engine, RTA and RPC client.',
            ['**One AR** (IOCARSingle). A second Connect is refused (`DB 81 40 04`), the same ARUUID again = re-connect.',
             '**Hooks**: the AR never owns the other engines, it starts and stops them.']),
          F('src/pn/pn_ar.cpp',
            'Connect validation and response, Write (incl. multiple write 0xE040 and PDev records on slot 0), '
            'Read/ReadImplicit (I&M0), PrmEnd, ApplicationReady with retries, timeouts, abort.',
            ['**Validation -> PNIOStatus**: RT class, send clock 32, reduction ratio (power of 2, >= `minUpdateTimeMs`), '
             'C_SDU 40..1440, every offset inside the C_SDU, exactly one AlarmCR. Field numbers are `[verify]`.',
             '**ApplicationReady after the PrmEnd answer**: `poll()` sends it on the RPC thread right after the '
             'response left, then retries every second until the activity timeout.',
             '**Release is not an abort**: `stats.arAborts` counts only real aborts.',
             '**Explicit null checks** for the code analyzer even where `validateConnect` already guarantees them.']),
          S('CMakeLists.txt', 'Add the AR', 'Append to the lists:',
            '''
                    src/pn/pn_ar.cpp      # SOFTPN_SOURCES
                    src/pn/pn_ar.hpp      # SOFTPN_HEADERS
            ''', ['**Next**: the facade wires all engines and threads.']),
      ],
      ['`softpn` builds with 0 warnings, also with `/analyze /WX` (Phase 9.16)',
       'Phase 9.15: happy path, wrong module, update time too fast, second AR, watchdog all pass'],
      architecture='''
       Idle --Connect--> WaitPrmEnd --Control(PrmEnd)--> WaitAppReadyCnf --AppReady.cnf--> Data
         ^                  | Write (records)               | ApplicationReady (retry 1 s)    |
         +------------ Release / watchdog / RTA ERR / activity timeout / stop() <-------------+
      ''')

phase('9.13', 'DeviceFacade', 'softpn::Device and pn_device_demo (Gate B)',
      'The public API: one class that opens the NIC, wires DCP, LLDP, RPC, AR, cyclic and RTA, owns the three '
      'threads, and offers `setInputData()` / `outputData()` like `softeip::Adapter`. Plus the demo device.',
      ['[softeip::Adapter usage](../../../README.md#using-the-library)'],
      [
          F('include/softpn/pn_device.hpp',
            'Public header (no winsock/pcap includes, pimpl). Names follow the PLC\'s view as in softeip: '
            '`setInputData` = PC -> PLC (slot 1), `outputData` = PLC -> PC (slot 2).',
            ['**States**: Stopped, WaitingForController, Connecting, DataRun, DataStop.',
             '**`port` / `rpcPort`**: test hooks (memory cable, a controller on the same PC).',
             '**Callbacks** run on stack threads: keep them short, never call `stop()` from one.']),
          F('src/pn/pn_device.cpp',
            'Wiring and threads: RX (dispatch by FrameID), cyclic (DeadlineTimer: input frames, watchdog, DCP '
            'delays, LLDP), RPC (requests, ApplicationReady, timeouts).',
            ['**Process image lives as long as the Device**: `setInputData()` before `start()` is kept.',
             '**`stop()`** sends an RTA ERR first (the controller sees a clean abort), then joins the threads.',
             '**TIME_CRITICAL** priority for RX and cyclic threads (option `raiseThreadPriority`).']),
          F('examples/pn_device_demo.cpp',
            'Echo device like `eip_adapter_demo`: outputs are echoed back from byte 4, bytes 0-3 are a heartbeat. '
            'Prints statistics every 10 s.',
            ['**`--promisc 1`** if CODESYS/Proneta cannot find the device on a given NIC.',
             '**`--priority 0`** to compare jitter with normal thread priority.']),
          S('CMakeLists.txt', 'Add the facade and the demo', 'Append to the lists and add the executable:',
            '''
                    src/pn/pn_device.cpp            # SOFTPN_SOURCES
                    include/softpn/pn_device.hpp    # SOFTPN_HEADERS

                add_executable(pn_device_demo examples/pn_device_demo.cpp)
                target_link_libraries(pn_device_demo PRIVATE softpn)
                softeip_warnings(pn_device_demo)
                set_target_properties(pn_device_demo PROPERTIES FOLDER "examples" VS_DEBUGGER_COMMAND_ARGUMENTS "--list")
            ''', ['**Linux**: build, then `sudo setcap cap_net_raw+ep build/linux/bin/pn_device_demo`.']),
          T('Gate B: discovery', 'The device must be visible and nameable before connections make sense.',
            '''
            ```bat
            pn_device_demo --adapter "Ethernet 2" --name softpn-device
            ```
            From the controller side (CODESYS "Scan for devices", Proneta, or `pn_probe --adapter X --identify`
            on a second PC): the device appears with vendor 0xFFFF / device 0x0001 and "Univision SoftPN".
            Set a new name (`pn_probe --set-name <MAC> line1-camera`), restart the demo: the name is kept
            (`softpn.ini`). `pn_probe --blink <MAC>` prints "DCP signal" in the demo.
            '''),
      ],
      ['`pn_device_demo --list` shows the adapters', 'Gate B: identify, set name (persisted), blink',
       'Wireshark: DCP Identify response and LLDP every 5 s without malformed markers'])

phase('9.14', 'ControllerSimulator', 'Controller simulator (pn_controller_sim)',
      'A minimal IO-Controller that runs the same startup as a PLC, to test the device without CODESYS: DCP '
      'Identify, Connect, Write, Read I&M0, PrmEnd + ApplicationReady, cyclic echo, Release, and error cases.',
      [SPEC + ' phase 9f', '[ip-netns(8)](https://man7.org/linux/man-pages/man8/ip-netns.8.html)'],
      [
          F('tools/pn_controller.hpp', 'Reusable class (also used by `pn_selftest`), one method per startup step.',
            ['**Fixed layout** matching the GSDML: slot 1 input, slot 2 output, DAP submodules.',
             '**Options**: wrong module, update time (RR), data-hold factor, VLAN, advanced startup.']),
          F('tools/pn_controller.cpp',
            'Builds the Connect request (AR, 2 IOCRs with frame offsets, AlarmCR, expected submodules), serves '
            'the device\'s ApplicationReady on its own RPC port, sends output frames with the same DeadlineTimer '
            'scheduling as the device, and counts echoes.',
            ['**Echo check** tolerates one cycle of lag after each data change.',
             '**RTA ERR detection**: proves the device notifies the controller when it aborts.']),
          F('tools/pn_controller_sim.cpp', 'Command-line front end with a step list and RESULT: PASS/FAIL.',
            ['**`--dhf N`**: tolerance of the device watchdog; the PLC default is 3.',
             '**`--rr 4`**: must be refused (`DB 81 02 0B`) with the default minimum of 8 ms.']),
          S('CMakeLists.txt', 'Build the simulator', 'Inside `if(SOFTPN_ENABLED)`:',
            '''
                # The controller simulator class is shared by the CLI tool and the offline self-test.
                add_library(pn_controller STATIC tools/pn_controller.cpp tools/pn_controller.hpp)
                target_include_directories(pn_controller PUBLIC src/pn tools)
                target_link_libraries(pn_controller PUBLIC softpn)
                softeip_warnings(pn_controller)

                add_executable(pn_controller_sim tools/pn_controller_sim.cpp)
                target_link_libraries(pn_controller_sim PRIVATE pn_controller)
                softeip_warnings(pn_controller_sim)
                set_target_properties(pn_controller pn_controller_sim PROPERTIES FOLDER "tools")
            ''', ['**Static helper library** instead of compiling `pn_controller.cpp` twice.']),
          T('Run it', 'Real Ethernet between device and simulator.',
            '''
            Two PCs (or two NICs cabled together): device on one, simulator on the other. Both work on Windows
            (Npcap) and Linux.

            - **Never the same NIC**: the device drops every frame that carries its own MAC (that is how it
              filters out its own transmissions), and the simulator would send with that same MAC.
            - **Two NICs in one PC** (e.g. a USB-Ethernet dongle) cabled to each other: give both NICs IPs in
              the same subnet. RPC then goes through Windows' own IP stack, the PROFINET frames over the cable.

            ```bat
            pn_device_demo    --adapter "Ethernet 2" --name softpn-device
            pn_controller_sim --adapter "Ethernet 3" --name softpn-device --seconds 60
            pn_controller_sim --adapter "Ethernet 3" --name softpn-device --rr 4            :: expect refusal
            pn_controller_sim --adapter "Ethernet 3" --name softpn-device --skip-release 1  :: device watchdog ends the AR
            pn_controller_sim --adapter "Ethernet 3" --name softpn-device --wrong-module 1  :: ModuleDiff, IOPS BAD
            ```
            Linux without hardware, as root, with two network namespaces joined by a veth pair:
            ```sh
            ip netns add pndev; ip netns add pnplc
            ip link add vdev type veth peer name vplc
            ip link set vdev netns pndev; ip link set vplc netns pnplc
            ip -n pndev addr add 192.168.77.10/24 dev vdev; ip -n pndev link set vdev up
            ip -n pnplc addr add 192.168.77.20/24 dev vplc; ip -n pnplc link set vplc up
            ip netns exec pndev ./pn_device_demo --adapter vdev --name veth-device &
            ip netns exec pnplc ./pn_controller_sim --adapter vplc --name veth-device --seconds 3
            ip netns del pndev; ip netns del pnplc
            ```
            A device on Windows with the simulator inside WSL (on the WSL virtual switch) checks the protocol, but
            not the timing: that path has 36-72 ms gaps, so use `--dhf 10` there.
            '''),
      ],
      ['Linux veth: all steps OK, cyclic >= 90 %, echo OK, RESULT: PASS',
       '`--rr 4` refused with `DB 81 02 0B`; `--skip-release 1` -> device log "data-hold timer expired"',
       'Two machines on real Ethernet: 60 s at 8 ms with the default `--dhf 3`, RESULT: PASS',
       'Simulator run on Windows (second PC or second NIC): RESULT: PASS'],
      results='''
      | Setup | Result |
      |---|---|
      | Linux veth + netns (raw AF_PACKET) | PASS: identify, connect, write, I&M0, PrmEnd/AppReady, 373/373 echoes in 3 s, release; `--rr 4` refused `DB 81 02 0B`; skip-release -> watchdog abort |
      | Windows device (Npcap on WSL vNIC), simulator in WSL | DCP, Connect, Write, Read, PrmEnd/AppReady, ModuleDiff, IOPS BAD OK; cyclic 1251/1251 with `--dhf 10` for 10 s. With `--dhf 3` the AR ends within ~1 s because the WSL path has gaps up to 36 ms (sender) / 72 ms (receiver) - a test-bed limit, not the device (device TX max 8.5-8.7 ms) |
      | Simulator on Windows (Npcap) | to do: so far it ran only on Linux / inside WSL; it builds on Windows with 0 warnings |
      | Two machines / CODESYS | to do |
      ''')

phase('9.15', 'SelfTest', 'Offline self-test (pn_selftest)',
      'Run the codecs and the complete device against the controller simulator in one process, over the '
      'memory cable and UDP loopback: no NIC, no Npcap, no PLC. This is the regression test for every change.',
      ['[CTest](https://cmake.org/cmake/help/latest/manual/ctest.1.html)'],
      [
          F('tools/pn_selftest.cpp',
            'Sections: NameOfStation rules, DCP codec, RPC header/NDR (both byte orders), PNIO blocks, GSDML ident '
            'numbers, end-to-end happy path, error paths (update time too fast, wrong module, watchdog + RTA ERR + '
            'reconnect).',
            ['**Unbuffered stdout**: if a test crashes, the output up to that point is kept.',
             '**The echo callback** captures a `shared_ptr`, not a stack variable: the first version captured a '
             'local by reference and crashed once `makeRig()` had returned (found with AddressSanitizer).']),
          S('CMakeLists.txt', 'Build and register the test', 'Inside `if(SOFTPN_ENABLED)`:',
            '''
                add_executable(pn_selftest tools/pn_selftest.cpp)
                target_link_libraries(pn_selftest PRIVATE pn_controller)
                target_compile_definitions(pn_selftest PRIVATE
                    SOFTPN_DEFAULT_GSDML="${CMAKE_CURRENT_SOURCE_DIR}/gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml")
                softeip_warnings(pn_selftest)
                enable_testing()
                add_test(NAME pn_selftest COMMAND pn_selftest)
                set_target_properties(pn_selftest PROPERTIES FOLDER "tools")
            ''', ['**`SOFTPN_DEFAULT_GSDML`**: the test finds the GSDML without arguments; `--gsdml` overrides it.']),
          T('Optional: AddressSanitizer run', 'Catches memory errors in the threaded end-to-end tests.',
            '''
            From PowerShell (Git Bash rewrites `/fsanitize` into a path):
            ```powershell
            cmake -S . -B build\\asan -G "Visual Studio 18 2026" -A x64 -DSOFTEIP_BUILD_DOTNET=OFF `
                  "-DCMAKE_CXX_FLAGS=/fsanitize=address /Zi /EHsc /DWIN32 /D_WINDOWS"
            cmake --build build\\asan --config Debug --target pn_selftest
            # copy clang_rt.asan_dynamic-x86_64.dll from VC\\Tools\\MSVC\\<ver>\\bin\\Hostx64\\x64 next to the exe
            build\\asan\\bin\\Debug\\pn_selftest.exe
            ```
            '''),
      ],
      ['`pn_selftest` prints `107 checks, 0 failed` and `RESULT: PASS` (Windows Debug/Release, ASan, Linux)',
       '`ctest -C Release` in the build folder passes'])

phase('9.16', 'FieldbusDeviceTransport', 'PROFINET in softfb::FieldbusDevice',
      'Make PROFINET a transport of the common Hilscher-style API, so applications switch fieldbus by '
      'configuration: `Transport::ProfinetRt` in `FieldbusDevice`, `fb_device_demo --transport profinet`, and '
      'PROFINET inside `SoftFieldbus.dll`.',
      ['[One API for every fieldbus](../../../README.md#one-api-for-every-fieldbus)'],
      [
          F('include/softfb/fieldbus_device.hpp',
            '`Transport::ProfinetRt` and `DeviceConfig::profinet` (adapter, station name, persist path, vendor/device '
            'ID, minimum update time, VLAN, promiscuous).',
            ['**Area mapping** (PC view): output area = slot 1 (PC -> PLC), input area = slot 2 (PLC -> PC).',
             '**`bindAddress` is not used** for PROFINET: the adapter selects the NIC.']),
          F('src/fieldbus_device.cpp',
            'Starts a `softpn::Device` for `ProfinetRt`, maps the states and the areas. Compiled with '
            '`SOFTFB_HAS_PROFINET`; without it, selecting PROFINET fails in `start()` with a clear message.',
            ['**State mapping**: DataRun -> ConnectedRun, DataStop -> ConnectedIdle, no AR / parameterising -> '
             'WaitingForMaster.',
             '**Shadow copy**: partial `ioWrite()` calls are merged before `setInputData()`, as for EtherNet/IP.']),
          F('examples/fb_device_demo.cpp', '`--transport profinet --adapter NIC --station NAME`.',
            ['**Sizes**: `--in-size` / `--out-size` must be a GSDML variant (default 64 is one).']),
          S('CMakeLists.txt', 'Link softpn into the common API and SoftFieldbus.dll',
            'At the end of `if(SOFTPN_ENABLED)`, and inside `if(SOFTFIELDBUS_SHARED)` before `softeip_warnings(softfieldbus_shared)`:',
            '''
                # PROFINET as a transport of the common API (softfb::FieldbusDevice, SoftFieldbus.Net).
                target_link_libraries(softfieldbus PUBLIC softpn)
                target_compile_definitions(softfieldbus PUBLIC SOFTFB_HAS_PROFINET=1)
            endif()

            # ... inside if(SOFTFIELDBUS_SHARED):
                if(SOFTPN_ENABLED)
                    target_sources(softfieldbus_shared PRIVATE ${SOFTPN_SOURCES})
                    target_include_directories(softfieldbus_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/pn)
                    target_compile_definitions(softfieldbus_shared PUBLIC SOFTFB_HAS_PROFINET=1)
                    if(WIN32)
                        target_include_directories(softfieldbus_shared PRIVATE "${SOFTPN_NPCAP_SDK}/Include")
                        target_link_libraries(softfieldbus_shared PRIVATE "${SOFTPN_NPCAP_SDK}/Lib/x64/wpcap.lib" iphlpapi delayimp)
                        target_link_options(softfieldbus_shared PRIVATE "/DELAYLOAD:wpcap.dll")
                    endif()
                endif()
            ''', ['**`SOFTFB_HAS_PROFINET` is PUBLIC**: consumers of the DLL see the same `#ifdef`s.']),
          F('CMakeLists.txt',
            'For reference: the complete file after all phases, to compare with yours.',
            ['**Build variants checked**: static `/W4`, `SoftFieldbus.dll` + `/analyze /WX`, .NET wrapper, no SDK, Linux.'],
            title='Reference: the complete CMakeLists.txt'),
      ],
      ['`fb_device_demo --transport profinet --adapter "Ethernet 2" --station softpn-device` reaches ConnectedRun against the simulator or CODESYS',
       'Build without `SOFTPN_NPCAP_SDK`: everything builds; `--transport profinet` prints "this build has no PROFINET support"',
       '`cmake --preset vs2026-analyze` with `-DSOFTPN_NPCAP_SDK=...` builds `SoftFieldbus.dll` with `/analyze /WX` clean',
       'EtherNet/IP and Modbus regression tests still pass'])

phase('9.17', 'DotNetWrapper', 'PROFINET in SoftFieldbus.Net (C#)',
      'Expose the transport to WPF / WinUI / MAUI applications: `FieldbusTransport.ProfinetRt` and the '
      '`Profinet*` configuration properties, plus the C# sample.',
      ['[SoftFieldbus.Net usage](../../../README.md#using-it-from-c-wpf--winui--maui-windows)'],
      [
          F('dotnet/SoftFieldbusNet.h', 'Enum value 4 and the configuration properties.',
            ['**Enum values mirror the native enum** (checked by a `static_assert` in the .cpp).']),
          F('dotnet/SoftFieldbusNet.cpp', 'Range checks and copying of the new properties; defaults come from the native config.',
            ['**Single source of defaults**: the managed config reads `softfb::DeviceConfig{}`.']),
          F('dotnet/fieldbus_sample/Program.cs', '`SoftFieldbusSample profinet "Ethernet 2" [station-name]`.',
            ['**Events on the network thread**: marshal to the UI thread in real applications.']),
          T('Build note', 'Command-line builds of the C# projects need a NuGet restore.',
            '''
            Visual Studio restores automatically. From the command line:
            ```bat
            cmake --build --preset vs2026-release -- /restore
            ```
            Deploy `SoftFieldbus.Net.dll` and `Ijwhost.dll`; Npcap must be installed on the target PC.
            '''),
      ],
      ['`SoftFieldbusSample profinet "Ethernet 2" softpn-device` reaches ConnectedRun and prints InputsChanged',
       'EtherNet/IP and Modbus modes of the sample unchanged'])

phase('9.18', 'CodesysBringUp', 'Windows setup and CODESYS bring-up (Gates C and D)',
      'Connect a real CODESYS PROFINET controller, resolve the `[verify]` items from captures, and run the soak '
      'test that decides go / no-go.',
      ['[CODESYS PROFINET](https://www.codesys.com/products/codesys-fieldbus/profinet.html)', WIRESHARK],
      [
          T('Windows PC (device side)', 'Most field problems are NIC settings, not code.',
            '''
            - Dedicated NIC for the machine network, **static IPv4** - the same IP you enter for the device in CODESYS
              (the PoC refuses a DCP Set IP to another address).
            - NIC properties: unbind **Microsoft LLDP Protocol Driver**; disable energy-efficient Ethernet / power
              saving and interrupt moderation; Intel "Packet Priority & VLAN": do not strip tags (or disable).
            - Firewall: allow inbound **UDP 34964** for the executable. DCP, RT and LLDP are Layer 2 (no firewall).
            - Npcap installed (free edition for the lab).
            - Run `pn_probe --jitter` on this NIC first (Gate A).
            '''),
          T('CODESYS controller', 'Separate hardware: a controller and device on the same NIC do not work through Npcap.',
            '''
            1. CODESYS Control for Linux SL or Raspberry Pi SL (demo mode is enough), Ethernet port cabled to the PC NIC.
            2. Tools -> Device Repository -> Install `gsdml/GSDML-V2.43-Univision-SoftPN-*.xml`.
            3. Device tree: Ethernet -> PROFINET IO Controller -> add "SoftPN device"; slot 1 "Input N bytes",
               slot 2 "Output N bytes" with N = the demo's sizes (`pn_device_demo` default 32).
            4. Device: NameOfStation = `--name`, IP = the PC NIC's IP; update time >= 8 ms.
            5. "Scan for devices" -> assign the name; download; start.
            6. Device side: `pn_device_demo --adapter "Ethernet 2" --name softpn-device` (or `fb_device_demo --transport profinet`).
            '''),
          T('Resolve the [verify] items', 'Values written from the IEC spec as decoded by Wireshark, not yet seen on a real controller.',
            '''
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
            '''),
          T('Gate C and Gate D', 'The go / no-go evidence for management.',
            '''
            - **Gate C**: CODESYS completes Connect -> Write -> PrmEnd -> ApplicationReady (device log "AR: Data").
            - **Gate D**: device green in CODESYS at 8, 16 and 32 ms; echo visible in the PLC; **1 h soak with
              `aborts 0`** in the demo statistics (and no AR loss in the CODESYS log); PLC STOP/RUN ->
              DataStop/DataRun; cable pull -> abort and automatic reconnect; restart of the demo -> reconnect.
            - Record max TX period, late cycles and aborts in `docs/phases/phase-9-profinet.md`.
            '''),
      ],
      ['Gate C passed with CODESYS', 'Gate D passed: 1 h, 0 aborts at 8 ms (else: at which update time it holds)',
       'All `[verify]` items resolved and the docs updated', 'Results page updated'])


# =====================================================================================================
# Rendering
# =====================================================================================================

def render_step(n, i, st):
    out = []
    num = f'{n}.{i}'
    if st['kind'] == 'file':
        title = st['title'] or f'`{os.path.basename(st["path"])}`'
        out.append(f'## Step {num}: {title}\n')
        out.append(f'**File:** `{st["path"]}`\n')
        out.append('### Purpose\n' + st['purpose'] + '\n')
        if st.get('note'):
            out.append(f'> {st["note"]}\n')
        out.append('### Implementation\n')
        out.append(fence(read(st['path']), lang_of(st['path'])) + '\n')
    elif st['kind'] == 'snippet':
        out.append(f'## Step {num}: {st["title"]}\n')
        out.append(f'**File:** `{st["path"]}`\n')
        out.append('### Purpose\n' + st['purpose'] + '\n')
        out.append('### Implementation\n')
        out.append(fence(st['code'], st['lang']) + '\n')
    else:
        out.append(f'## Step {num}: {st["title"]}\n')
        out.append('### Purpose\n' + st['purpose'] + '\n')
        out.append('### Procedure\n' + st['body'] + '\n')
    if st.get('concepts'):
        out.append('### Key Concepts')
        for k, c in enumerate(st['concepts'], 1):
            out.append(f'{k}. {c}')
        out.append('')
    out.append('---\n')
    return '\n'.join(out)


def fname(p):
    return f'Phase{p["num"]}-{p["slug"]}.md'


def render_phase(idx, p):
    lines = [f'# Phase {p["num"]}: {p["title"]}\n',
             '> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). '
             'Code blocks are the verified files from branch `feature/profinet-poc`.\n',
             '## Goal\n' + p['goal'] + '\n',
             '## Documentation References']
    lines += [f'- {r}' for r in p['refs']]
    lines.append('\n---\n')
    for i, st in enumerate(p['steps'], 1):
        lines.append(render_step(p['num'], i, st))
    if p['architecture']:
        lines.append('## Architecture\n')
        lines.append(fence(textwrap.dedent(p['architecture']).strip('\n'), '') + '\n\n---\n')
    if p['results']:
        lines.append('## Measured results\n')
        lines.append(textwrap.dedent(p['results']).strip('\n') + '\n\n---\n')
    lines.append('## Verification Checklist\n')
    lines += [f'- [ ] {v}' for v in p['verify']]
    lines.append('\n---\n')
    lines.append('## Next Phase')
    if idx + 1 < len(PHASES):
        nxt = PHASES[idx + 1]
        lines.append(f'Continue to [{fname(nxt)}]({fname(nxt)}): {nxt["title"]}.')
    else:
        lines.append('This is the last phase. Update [phase-9-profinet.md](../phase-9-profinet.md) with the results.')
    return '\n'.join(lines) + '\n'


def render_index():
    rows = []
    milestone = {'9.01': 'A - L2 access and timing', '9.06': 'B/C - LLDP, RPC, blocks, model',
                 '9.10': 'D - cyclic, alarms, AR, device', '9.14': 'E - test tools',
                 '9.16': 'F - common API and .NET', '9.18': 'G - CODESYS bring-up'}
    for p in PHASES:
        if p['num'] in milestone:
            rows.append(f'| **{milestone[p["num"]]}** | | |')
        files = [s['path'] for s in p['steps'] if s['kind'] == 'file']
        rows.append(f'| [{p["num"]}]({fname(p)}) | {p["title"]} | {", ".join("`" + os.path.basename(f) + "`" for f in files) or "-"} |')
    return f'''# softpn: PROFINET RT device PoC - phase documents

Step-by-step implementation of the in-house PROFINET RT IO-Device (Conformance Class A, RT_CLASS_1) for
Windows (Npcap) and Linux (AF_PACKET), generated with `/spec-to-code` from [the spec](../../profinet-spec.md).

**Every code block is the verified file from branch `feature/profinet-poc`**: built with 0 warnings
(MSVC `/W4`, `/analyze /WX` for `SoftFieldbus.dll`, g++ 13 `-Wall -Wextra -Wpedantic`) and tested with
`pn_selftest` (107 checks), a Linux veth test and a Windows Npcap test against the controller simulator.
Results: [phase-9-profinet.md](../phase-9-profinet.md).

The phases follow the include dependencies of the code, so each phase compiles on top of the previous
ones. The spec planned 24 phases; codecs and their users were merged where a file could not be split
without duplicating code, giving 18.

| Phase | Title | Files |
|---|---|---|
{chr(10).join(rows)}

## Gates
- **A** (9.05): Windows TX timing on the station NIC - `pn_probe --jitter`.
- **B** (9.13): device found and named by CODESYS / Proneta / `pn_probe --identify`.
- **C** (9.18): CODESYS completes the startup up to ApplicationReady.
- **D** (9.18): green at 8/16/32 ms, 1 h soak with 0 aborts, STOP/RUN, cable pull.

## Licensing reminder
Free Npcap: development and this PoC, at most 5 systems. Shipping on customer machines needs Npcap OEM
(management decision pending). The `L2Port` interface keeps an NDISPROT-based backend possible.
'''


def main():
    os.makedirs(OUT, exist_ok=True)
    for i, p in enumerate(PHASES):
        with open(os.path.join(OUT, fname(p)), 'w', encoding='utf-8', newline='\n') as f:
            f.write(render_phase(i, p))
    with open(os.path.join(OUT, 'README.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(render_index())
    # Every source file of the PoC must appear in exactly one phase.
    shown = [s['path'] for p in PHASES for s in p['steps'] if s['kind'] == 'file']
    expected = [os.path.join(d, f).replace('\\', '/') for d in ('include/softpn', 'src/pn') for f in os.listdir(d)]
    expected += ['tools/' + f for f in os.listdir('tools') if f.startswith('pn_')]
    expected += ['examples/pn_device_demo.cpp'] + ['gsdml/' + f for f in os.listdir('gsdml')]
    missing = sorted(set(expected) - set(shown))
    dup = sorted({x for x in shown if shown.count(x) > 1})
    print(f'{len(PHASES)} phases written to {OUT}; missing: {missing or "none"}; duplicated: {dup or "none"}')


main()
