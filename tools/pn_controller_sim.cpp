// Minimal PROFINET IO-Controller (PLC side) to test a softpn device over a real NIC.
// It runs the same sequence as a PLC: DCP Identify, Connect, Write, Read I&M0, PrmEnd,
// ApplicationReady, cyclic exchange with an echo check (against pn_device_demo / fb_device_demo),
// Release.
//
// Usage: pn_controller_sim --adapter <name|mac|ip> [--name softpn-device] [--set-name-mac MAC]
//                          [--in-size 32] [--out-size 32] [--rr 8] [--seconds 5] [--local-port 34965]
//                          [--device-port 34964] [--wrong-module 1] [--advanced 1] [--skip-release 1] [--vlan 0]
//                          [--dhf 3]   data-hold factor: frames the device may miss before it aborts
//                          [--backend auto|npcap|ndis]   Windows raw-Ethernet backend
//
// Run it on a second PC (or a second NIC cabled to the device NIC). On the same NIC as the device,
// Npcap's loopback of sent frames may also work [verify]; RPC then goes over the local IP stack.
#include "softeip/socket_compat.hpp" // first (winsock2.h before windows.h)

#include "pn_controller.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace softpn;

int main(int argc, char** argv)
{
    // WSAStartup for the RPC socket (so far the simulator ran on Linux or next to a device in the
    // same process, which had already started Winsock).
    softeip::SocketLibrary sockets;
    ControllerConfig cc;
    std::string adapter;
    std::string setNameMac;
    int seconds = 5;
    bool skipRelease = false;
    L2Backend backend = L2Backend::Auto;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        unsigned long n = std::strtoul(v, nullptr, 0);
        if (k == "--adapter") adapter = v;
        else if (k == "--name") cc.deviceName = v;
        else if (k == "--set-name-mac") setNameMac = v;
        else if (k == "--in-size") cc.inputSize = n;
        else if (k == "--out-size") cc.outputSize = n;
        else if (k == "--rr") cc.reductionRatio = uint16_t(n);
        else if (k == "--seconds") seconds = int(n);
        else if (k == "--local-port") cc.localRpcPort = uint16_t(n);
        else if (k == "--device-port") cc.deviceRpcPort = uint16_t(n);
        else if (k == "--wrong-module") cc.wrongModule = n != 0;
        else if (k == "--advanced") cc.advancedStartup = n != 0;
        else if (k == "--vlan") cc.vlan = n != 0;
        else if (k == "--dhf") cc.dataHoldFactor = uint16_t(n);
        else if (k == "--skip-release") skipRelease = n != 0;
        else if (k == "--backend") {
            if (!parseL2Backend(v, backend)) {
                std::fprintf(stderr, "--backend: auto, npcap or ndis\n");
                return 2;
            }
        }
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }
    if (adapter.empty()) {
        std::fprintf(stderr, "--adapter is required (pn_probe --list shows the adapters)\n");
        return 2;
    }
    std::string err;
    L2Options opt;
    opt.backend = backend;
    std::unique_ptr<L2Port> port = openL2Port(adapter, opt, &err);
    if (!port) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    cc.port = std::move(port);
    cc.log = [](const std::string& m) { std::printf("%s\n", m.c_str()); };
    ControllerSim plc(cc);
    if (!plc.start(&err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }

    int failures = 0;
    auto step = [&](bool ok, const char* what) {
        std::printf("%-28s %s\n", what, ok ? "OK" : "FAILED");
        if (!ok)
            ++failures;
        return ok;
    };

    if (!setNameMac.empty()) {
        MacAddress mac;
        step(MacAddress::parse(setNameMac, mac) && plc.setName(mac, cc.deviceName), "DCP Set NameOfStation");
    }
    if (step(plc.identify(), "DCP Identify")) {
        const PnioStatus st = plc.connect();
        if (step(st.ok(), "RPC Connect")) {
            if (!plc.moduleDiff().empty())
                std::printf("  ModuleDiffBlock with %zu module(s)\n", plc.moduleDiff().size());
            step(plc.writeRecords(), "RPC Write (PDev record)");
            Im0Data im0;
            if (step(plc.readIm0(im0), "RPC Read I&M0"))
                std::printf("  I&M0: vendor 0x%04X, order '%s', serial '%s'\n", im0.vendorId, im0.orderId.c_str(),
                            im0.serialNumber.c_str());
            if (step(plc.prmEnd(), "PrmEnd + ApplicationReady")) {
                plc.exchange(seconds * 1000);
                std::printf("  cyclic: sent %llu, received %llu, echo matches %llu, device IOPS %s\n",
                            (unsigned long long)plc.framesSent(), (unsigned long long)plc.framesReceived(),
                            (unsigned long long)plc.echoMatches(), plc.inputIopsGood() ? "GOOD" : "BAD");
                const uint64_t expected = uint64_t(seconds) * 1000 / cc.reductionRatio;
                step(plc.framesReceived() * 10 >= expected * 9, "cyclic frames >= 90 %");
                step(cc.wrongModule || plc.echoMatches() * 2 >= plc.framesReceived(), "echo (with pn_device_demo)");
            }
            if (!skipRelease)
                step(plc.release(), "RPC Release");
            else
                std::printf("skipping Release: the device watchdog must end the AR\n");
        } else {
            std::printf("  PNIOStatus %s\n", st.str().c_str());
        }
    }
    plc.stop();
    std::printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
