// Offline tests of the softpn PROFINET stack: no NIC, no Npcap, no PLC needed.
//
//  1. codec tests: DCP, NameOfStation rules, RPC header (both byte orders), NDR, blocks, I&M0
//  2. GSDML check: the module ident numbers in the GSDML match the device model (pn_model.cpp);
//     the ProInspect GSDML (process-image service) has only the 256-byte modules, fixed in slots 1 and 2
//  3. end-to-end: softpn::Device and ControllerSim joined by an in-memory cable (RPC over
//     UDP loopback): identify, set name, connect, records, PrmEnd/ApplicationReady, cyclic echo,
//     release, and the error paths (wrong module, update time too fast, second AR, watchdog)
//
// Usage: pn_selftest [--gsdml path/to/GSDML.xml] [--gsdml-proinspect path/to/ProInspect GSDML.xml]
//        prints RESULT: PASS / FAIL
#include "pn_controller.hpp"

#include "softpn/pn_device.hpp"

#include "pn_blocks.hpp"
#include "pn_dcp.hpp"
#include "pn_defs.hpp"
#include "pn_lldp.hpp"
#include "pn_model.hpp"
#include "pn_rpc.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace softpn;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
        }                                                                                                              \
    } while (0)

void section(const char* name) { std::printf("[%s]\n", name); }

// ---- 1. codecs -----------------------------------------------------------------------------------

void testStationNames()
{
    section("NameOfStation rules");
    CHECK(isValidStationName(""));
    CHECK(isValidStationName("vision-station-1"));
    CHECK(isValidStationName("line1.cell-2.camera"));
    CHECK(!isValidStationName("Vision"));        // uppercase
    CHECK(!isValidStationName("-station"));      // leading '-'
    CHECK(!isValidStationName("station-"));      // trailing '-'
    CHECK(!isValidStationName("a..b"));          // empty label
    CHECK(!isValidStationName("192.168.0.1"));   // looks like an IP
    CHECK(!isValidStationName("port-001"));      // reserved
    CHECK(!isValidStationName("port-001-00042"));
    CHECK(isValidStationName("port-01"));        // not the reserved pattern
    CHECK(!isValidStationName(std::string(64, 'a')));
}

void testDcpCodec()
{
    section("DCP codec");
    DcpPdu p;
    p.frameId = kFrameIdDcpGetSet;
    p.serviceId = dcp::kServiceSet;
    p.serviceType = dcp::kTypeRequest;
    p.xid = 0x12345678;
    p.blocks.push_back(makeDcpBlock(dcp::kOptDeviceProperties, dcp::kSubDevNameOfStation, 1, std::string("abc")));
    p.blocks.push_back(makeDcpBlock(dcp::kOptControl, dcp::kSubCtrlSignal, 0, "\x01\x00", 2));
    const std::vector<uint8_t> bytes = serializeDcp(p);
    // header 12 (FrameID 2, ServiceID, ServiceType, Xid 4, ResponseDelay 2, DCPDataLength 2)
    // + block1 (4 + 5 + pad 1) + block2 (4 + 4)
    CHECK(bytes.size() == 12 + 10 + 8);
    CHECK(bytes[10] == 0 && bytes[11] == 18); // DCPDataLength
    DcpPdu q = parseDcp(bytes.data(), bytes.size());
    CHECK(q.xid == 0x12345678);
    CHECK(q.blocks.size() == 2);
    CHECK(dcpQualifier(q.blocks[0]) == 1);
    const auto name = dcpPayload(q.blocks[0]);
    CHECK(std::string(name.begin(), name.end()) == "abc");

    // Get request: bare option/suboption pairs.
    DcpPdu g;
    g.frameId = kFrameIdDcpGetSet;
    g.serviceId = dcp::kServiceGet;
    g.serviceType = dcp::kTypeRequest;
    DcpBlock b;
    b.option = dcp::kOptIp;
    b.suboption = dcp::kSubIpParameter;
    b.bare = true;
    g.blocks.push_back(b);
    const auto gb = serializeDcp(g);
    CHECK(gb.size() == 14);
    DcpPdu g2 = parseDcp(gb.data(), gb.size());
    CHECK(g2.blocks.size() == 1 && g2.blocks[0].bare && g2.blocks[0].suboption == dcp::kSubIpParameter);

    bool threw = false;
    try {
        const uint8_t bad[] = {0xFE, 0xFD, 4, 0, 0, 0, 0, 1, 0, 0, 0, 50}; // length beyond frame
        parseDcp(bad, sizeof bad);
    } catch (const ParseError&) {
        threw = true;
    }
    CHECK(threw);
}

void testRpcCodec()
{
    section("RPC header / NDR");
    RpcHeader h;
    h.ptype = rpc::kPtypeRequest;
    h.flags1 = rpc::kFlag1LastFrag;
    h.object = pnioDeviceObject(1, 0x0001, 0xFFFF);
    h.interfaceId = pnioDeviceInterface();
    h.activity = Uuid::random();
    h.seqnum = 7;
    h.opnum = rpc::kOpConnect;
    ByteWriter w;
    writeRpcHeader(w, h, 20);
    CHECK(w.size() == rpc::kHeaderSize);
    w.zeros(20);
    RpcHeader r = parseRpcHeader(w.data().data(), w.size());
    CHECK(r.littleEndian && r.seqnum == 7 && r.opnum == rpc::kOpConnect && r.bodyLength == 20);
    CHECK(r.interfaceId == pnioDeviceInterface());
    CHECK(r.object == h.object);
    CHECK(r.activity == h.activity);

    // Big-endian header as some controllers send it: flip drep and the numeric fields.
    std::vector<uint8_t> be = w.data();
    be[4] = 0x00;
    auto swap32 = [&](size_t o) {
        std::swap(be[o], be[o + 3]);
        std::swap(be[o + 1], be[o + 2]);
    };
    auto swap16 = [&](size_t o) { std::swap(be[o], be[o + 1]); };
    for (size_t uuid : {8u, 24u, 40u}) {
        swap32(uuid);
        swap16(uuid + 4);
        swap16(uuid + 6);
    }
    for (size_t o : {56u, 60u, 64u})
        swap32(o);
    for (size_t o : {68u, 70u, 72u, 74u, 76u})
        swap16(o);
    RpcHeader rb = parseRpcHeader(be.data(), be.size());
    CHECK(!rb.littleEndian && rb.seqnum == 7 && rb.bodyLength == 20 && rb.interfaceId == pnioDeviceInterface());

    const std::vector<uint8_t> blocks = {1, 2, 3, 4, 5};
    const auto req = buildNdrRequest(1000, blocks);
    NdrArgs a = parseNdrRequest(req.data(), req.size(), true);
    CHECK(a.argsMaximum == 1000 && a.blocks == blocks);
    const auto res = buildNdrResponse(PnioStatus::make(0xDB, 0x81, 0x40, 0x04), 1000, blocks);
    NdrArgs b = parseNdrResponse(res.data(), res.size(), true);
    CHECK(b.status.code == 0xDB && b.status.code2 == 0x04 && b.blocks == blocks);
}

void testBlocks()
{
    section("PNIO blocks");
    ArBlockReq ar;
    ar.arType = kArTypeIocarSingle;
    ar.arUuid = Uuid::random();
    ar.sessionKey = 3;
    MacAddress::parse("02:00:00:00:00:02", ar.cmInitiatorMac);
    ar.cmInitiatorObjectUuid = pnioDeviceObject(1, 2, 3);
    ar.arProperties = 0x40000001;
    ar.activityTimeoutFactor = 600;
    ar.cmInitiatorUdpPort = 34964;
    ar.cmInitiatorStationName = "plc";
    ByteWriter w;
    writeArBlockReq(w, ar);
    size_t used = 0;
    RawBlock rb = readBlock(w.data().data(), w.size(), used);
    CHECK(used == w.size());
    CHECK(rb.type == block::kArBlockReq && rb.versionHigh == 1);
    ArBlockReq ar2 = parseArBlockReq(rb);
    CHECK(ar2.arUuid == ar.arUuid && ar2.cmInitiatorStationName == "plc" && ar2.arProperties == 0x40000001);
    CHECK(ar2.cmInitiatorMac == ar.cmInitiatorMac);

    ByteWriter w2;
    RecordHeader h;
    h.type = block::kWriteResHeader;
    h.index = 0x802F;
    h.status = PnioStatus::make(0xDF, 0x80, 0xB0, 0);
    writeRecordHeader(w2, h);
    CHECK(w2.size() == block::kRecordHeaderLength + 4);
    RecordHeader h2 = parseRecordHeader(readBlock(w2.data().data(), w2.size(), used));
    CHECK(h2.index == 0x802F && h2.status.code1 == 0xB0);

    ByteWriter w3;
    Im0Data im;
    im.vendorId = 0x1234;
    im.orderId = "ORDER";
    im.serialNumber = "SN1";
    im.swRevision[1] = 2;
    writeIm0Block(w3, im);
    CHECK(w3.size() == 60); // 6-byte header + 54 bytes
    Im0Data im2 = parseIm0Block(readBlock(w3.data().data(), w3.size(), used));
    CHECK(im2.vendorId == 0x1234 && im2.orderId.substr(0, 5) == "ORDER" && im2.orderId.size() == 20);
    CHECK(im2.swRevision[1] == 2 && im2.imVersion == 0x0101);

    // ModuleDiff for a wrong slot-1 module
    DeviceModel model;
    CHECK(model.build(32, 64));
    ExpectedModule wrong{0, kSlotInput, 0x99, 0, {{1, kIoSubmoduleIdent, 1, {{1, 32, 1, 1}}}}};
    ExpectedModule good{0, kSlotOutput, findModuleVariant(64)->outputModuleIdent, 0,
                        {{1, kIoSubmoduleIdent, 2, {{2, 64, 1, 1}}}}};
    ExpectedModule missing{0, 7, 0x10, 0, {}};
    ConfigCheck cc = compareExpected(model, {wrong, good, missing});
    CHECK(cc.diff.size() == 2);
    CHECK(cc.diff[0].slot == kSlotInput && cc.diff[0].state == kModuleStateWrong);
    CHECK(cc.diff[1].slot == 7 && cc.diff[1].state == kModuleStateNoModule);
    CHECK(cc.proper.count(submoduleKey(0, kSlotOutput, 1)) == 1);
    CHECK(cc.proper.count(submoduleKey(0, kSlotInput, 1)) == 0);
    ExpectedModule wrongLen{0, kSlotOutput, findModuleVariant(64)->outputModuleIdent, 0,
                            {{1, kIoSubmoduleIdent, 2, {{2, 32, 1, 1}}}}};
    CHECK(compareExpected(model, {wrongLen}).diff.size() == 1);
}

void testLldp()
{
    section("LLDP MAU type");
    CHECK(lldp::mauTypeFor(1000, true) == 30 && lldp::mauTypeFor(100, true) == 16);
    CHECK(lldp::mauTypeFor(100, false) == 15 && lldp::mauTypeFor(10, true) == 11);
    CHECK(lldp::mauTypeFor(0, true) == 0 && lldp::mauTypeFor(2500, true) == 0); // unknown: keep the default

    // IEEE 802.3 MAC/PHY TLV: OUI 00-12-0F, subtype 1, autoneg, advertised capability, MAU type.
    auto macPhy = [](uint16_t mau) {
        LldpInfo li;
        MacAddress::parse("02:00:00:00:00:01", li.mac);
        li.stationName = "dev";
        li.mauType = mau;
        const std::vector<uint8_t> f = buildLldpFrame(li);
        const uint8_t key[4] = {0x00, 0x12, 0x0F, 0x01};
        for (size_t i = 0; i + 9 <= f.size(); ++i)
            if (std::memcmp(&f[i], key, 4) == 0)
                return std::vector<uint8_t>(f.begin() + long(i) + 4, f.begin() + long(i) + 9);
        return std::vector<uint8_t>{};
    };
    CHECK((macPhy(30) == std::vector<uint8_t>{0x03, 0x6C, 0x01, 0x00, 30}));
    CHECK((macPhy(16) == std::vector<uint8_t>{0x03, 0x6C, 0x00, 0x00, 16}));
}

// ---- 2. GSDML ------------------------------------------------------------------------------------

void testGsdml(const std::string& path)
{
    section("GSDML ident numbers");
    std::ifstream f(path);
    if (!f) {
        std::printf("  skipped: %s not found (pass --gsdml)\n", path.c_str());
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string xml = ss.str();
    auto has = [&](const std::string& s) { return xml.find(s) != std::string::npos; };
    char buf[64];
    for (const ModuleVariant& v : moduleVariants()) {
        std::snprintf(buf, sizeof buf, "ModuleIdentNumber=\"0x%08X\"", v.inputModuleIdent);
        CHECK(has(buf));
        std::snprintf(buf, sizeof buf, "ModuleIdentNumber=\"0x%08X\"", v.outputModuleIdent);
        CHECK(has(buf));
        std::snprintf(buf, sizeof buf, "Length=\"%zu\"", v.bytes);
        CHECK(has(buf));
    }
    std::snprintf(buf, sizeof buf, "ModuleIdentNumber=\"0x%08X\"", kDapModuleIdent);
    CHECK(has(buf));
    CHECK(has("SubmoduleIdentNumber=\"0x00008000\""));
    CHECK(has("SubmoduleIdentNumber=\"0x00008001\""));
    CHECK(has("VendorID=\"0xFFFF\"") && has("DeviceID=\"0x0001\""));
}

void testProInspectGsdml(const std::string& path)
{
    section("ProInspect GSDML");
    std::ifstream f(path);
    if (!f) {
        std::printf("  skipped: %s not found (pass --gsdml-proinspect)\n", path.c_str());
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string xml = ss.str();
    auto has = [&](const std::string& s) { return xml.find(s) != std::string::npos; };
    char buf[64];
    // only the 256-byte modules: the service always runs with 256 / 256 bytes
    for (const ModuleVariant& v : moduleVariants()) {
        const bool used = v.bytes == 256;
        std::snprintf(buf, sizeof buf, "ModuleIdentNumber=\"0x%08X\"", v.inputModuleIdent);
        CHECK(has(buf) == used);
        std::snprintf(buf, sizeof buf, "ModuleIdentNumber=\"0x%08X\"", v.outputModuleIdent);
        CHECK(has(buf) == used);
    }
    CHECK(has("Length=\"256\""));
    std::snprintf(buf, sizeof buf, "ModuleIdentNumber=\"0x%08X\"", kDapModuleIdent);
    CHECK(has(buf));
    CHECK(has("SubmoduleIdentNumber=\"0x00008000\""));
    CHECK(has("SubmoduleIdentNumber=\"0x00008001\""));
    CHECK(has("VendorID=\"0xFFFF\"") && has("DeviceID=\"0x0002\""));
    CHECK(has("ModuleItemTarget=\"IN256\" AllowedInSlots=\"1\" FixedInSlots=\"1\""));
    CHECK(has("ModuleItemTarget=\"OUT256\" AllowedInSlots=\"2\" FixedInSlots=\"2\""));
}

// ---- 3. end-to-end -------------------------------------------------------------------------------

struct Rig {
    std::shared_ptr<L2Port> devicePort, controllerPort;
    std::unique_ptr<Device> device;
    std::unique_ptr<ControllerSim> plc;
    std::string iniPath;
};

AdapterInfo memoryAdapter(const char* name, const char* mac)
{
    AdapterInfo a;
    a.name = a.friendlyName = name;
    MacAddress::parse(mac, a.mac);
    a.ipv4 = 0x7F000001; // 127.0.0.1: RPC goes over UDP loopback
    a.netmask = 0xFF000000;
    return a;
}

bool makeRig(Rig& rig, ControllerConfig cc, const std::string& stationName, bool verbose)
{
    auto cable = makeMemoryCable(memoryAdapter("mem-dev", "02:00:00:00:00:01"),
                                 memoryAdapter("mem-plc", "02:00:00:00:00:02"));
    rig.devicePort = std::move(cable.first);
    rig.controllerPort = std::move(cable.second);
    rig.iniPath = "pn_selftest_station.ini";
    std::remove(rig.iniPath.c_str());

    DeviceConfig dc;
    dc.port = rig.devicePort;
    dc.stationName = stationName;
    dc.persistPath = rig.iniPath;
    dc.inputSize = 32;
    dc.outputSize = 32;
    dc.raiseThreadPriority = false;
    cc.localRpcPort = 34965; // device and controller share the loopback interface
    dc.controllerRpcPort = cc.localRpcPort;
    auto self = std::make_shared<Device*>(nullptr); // outlives makeRig(): the callback runs later
    dc.onOutputs = [self](const std::vector<uint8_t>& out, bool) {
        if (*self)
            (*self)->setInputData(out.data(), out.size()); // echo application
    };
    if (verbose)
        dc.onLog = [](const std::string& m) { std::printf("    dev: %s\n", m.c_str()); };
    rig.device = std::make_unique<Device>(dc);
    *self = rig.device.get();
    std::string err;
    if (!rig.device->start(&err)) {
        std::printf("  device start failed: %s\n", err.c_str());
        return false;
    }
    cc.port = rig.controllerPort;
    if (verbose)
        cc.log = [](const std::string& m) { std::printf("    plc: %s\n", m.c_str()); };
    rig.plc = std::make_unique<ControllerSim>(cc);
    if (!rig.plc->start(&err)) {
        std::printf("  controller start failed: %s\n", err.c_str());
        return false;
    }
    return true;
}

void endRig(Rig& rig)
{
    if (rig.plc)
        rig.plc->stop();
    if (rig.device)
        rig.device->stop();
    std::remove(rig.iniPath.c_str());
}

bool waitFor(const std::function<bool()>& cond, int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

void testHappyPath(bool verbose)
{
    section("end-to-end: name, connect, records, PrmEnd, cyclic echo, release");
    Rig rig;
    ControllerConfig cc;
    cc.deviceName = "selftest-device";
    cc.advancedStartup = true;
    if (!makeRig(rig, cc, "", verbose)) {
        CHECK(false);
        return;
    }
    ControllerSim& plc = *rig.plc;
    Device& dev = *rig.device;

    // Unnamed device: Identify by name fails, set the name by MAC, then Identify works.
    CHECK(!plc.identify(300));
    CHECK(plc.setName(rig.devicePort->info().mac, "selftest-device"));
    CHECK(dev.stationName() == "selftest-device");
    StationData persisted;
    CHECK(StationStore(rig.iniPath).load(persisted) && persisted.name == "selftest-device");
    CHECK(!plc.setName(rig.devicePort->info().mac, "Bad_Name")); // refused (BlockError)
    CHECK(plc.identify());
    CHECK(plc.device().ip == 0x7F000001 && plc.device().vendorId == 0xFFFF);

    CHECK(plc.connect().ok());
    CHECK(plc.moduleDiff().empty());
    CHECK(plc.arServerBlockSeen());
    CHECK(dev.state() == DeviceState::Connecting);
    CHECK(plc.writeRecords());
    Im0Data im0;
    CHECK(plc.readIm0(im0));
    CHECK(im0.vendorId == 0xFFFF && im0.orderId.substr(0, 8) == "SOFTPN-1");
    CHECK(plc.prmEnd());
    CHECK(waitFor([&] { return dev.state() == DeviceState::DataRun; }, 1000));
    CHECK(plc.connectSecondAr().code1 == err::kCode1Cmrpc); // only one AR

    plc.exchange(1000);
    std::printf("  frames sent %llu, received %llu, echo matches %llu\n", (unsigned long long)plc.framesSent(),
                (unsigned long long)plc.framesReceived(), (unsigned long long)plc.echoMatches());
    CHECK(plc.framesReceived() > 60); // 8 ms update time for ~1 s
    CHECK(plc.echoMatches() > plc.framesReceived() / 2);
    CHECK(plc.inputIopsGood());
    const auto out = dev.outputData();
    CHECK(out.size() == 32 && out[5] != 0);

    CHECK(plc.release());
    CHECK(waitFor([&] { return dev.state() == DeviceState::WaitingForController; }, 500));
    const DeviceStats st = dev.stats();
    CHECK(st.arConnects == 1 && st.arAborts == 0);
    std::printf("  device: tx %llu rx %llu, max TX period %u us, late cycles %llu\n",
                (unsigned long long)st.txFrames, (unsigned long long)st.rxFrames, st.maxTxPeriodUs,
                (unsigned long long)st.lateCycles);
    endRig(rig);
}

void testErrorPaths(bool verbose)
{
    section("end-to-end: error paths");
    {
        Rig rig;
        ControllerConfig cc;
        cc.deviceName = "dev-a";
        cc.reductionRatio = 4; // 4 ms < minUpdateTimeMs 8
        if (makeRig(rig, cc, "dev-a", verbose)) {
            CHECK(rig.plc->identify());
            PnioStatus st = rig.plc->connect();
            CHECK(st.code == err::kCodeConnect && st.code1 == err::kCode1IocrBlock && st.code2 == 11);
            CHECK(rig.device->state() == DeviceState::WaitingForController);
        } else {
            CHECK(false);
        }
        endRig(rig);
    }
    {
        Rig rig;
        ControllerConfig cc;
        cc.deviceName = "dev-b";
        cc.wrongModule = true;
        if (makeRig(rig, cc, "dev-b", verbose)) {
            CHECK(rig.plc->identify());
            CHECK(rig.plc->connect().ok()); // the AR is accepted, the module is reported
            CHECK(rig.plc->moduleDiff().size() == 1 && rig.plc->moduleDiff()[0].slot == kSlotInput);
            CHECK(rig.plc->prmEnd());
            rig.plc->exchange(300);
            CHECK(!rig.plc->inputIopsGood()); // wrong submodule: IOPS BAD
            CHECK(rig.plc->release());
        } else {
            CHECK(false);
        }
        endRig(rig);
    }
    {
        Rig rig;
        ControllerConfig cc;
        cc.deviceName = "dev-c";
        if (makeRig(rig, cc, "dev-c", verbose)) {
            CHECK(rig.plc->identify());
            CHECK(rig.plc->connect().ok());
            CHECK(rig.plc->prmEnd());
            CHECK(waitFor([&] { return rig.device->state() == DeviceState::DataRun; }, 1000));
            rig.plc->stopCyclic(); // dead controller: data-hold time = 3 x 8 ms
            CHECK(waitFor([&] { return rig.device->state() == DeviceState::WaitingForController; }, 500));
            CHECK(rig.device->stats().arAborts == 1);
            CHECK(waitFor([&] { return rig.plc->rtaErrorReceived(); }, 200)); // device sent RTA ERR
            // A new AR after the abort works.
            CHECK(rig.plc->connect().ok());
            CHECK(rig.plc->prmEnd());
            CHECK(rig.plc->release());
        } else {
            CHECK(false);
        }
        endRig(rig);
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep the output if a test crashes
    std::string gsdml = SOFTPN_DEFAULT_GSDML;
    std::string gsdmlProInspect = SOFTPN_PROINSPECT_GSDML;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--gsdml" && i + 1 < argc)
            gsdml = argv[++i];
        else if (a == "--gsdml-proinspect" && i + 1 < argc)
            gsdmlProInspect = argv[++i];
        else if (a == "--verbose")
            verbose = true;
    }
    testStationNames();
    testDcpCodec();
    testRpcCodec();
    testBlocks();
    testLldp();
    testGsdml(gsdml);
    testProInspectGsdml(gsdmlProInspect);
    testHappyPath(verbose);
    testErrorPaths(verbose);
    std::printf("%d checks, %d failed\nRESULT: %s\n", g_checks, g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
