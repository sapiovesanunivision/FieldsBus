# Phase 10b: GSDML for the ProInspect service

> Part of [Phase 10](../phase-10-dev3-profinet.md). Index: [README.md](README.md). Repository: **FieldsBus**,
> branch `feature/profinet-poc`. **Status: implemented 2026-10-21** (uncommitted); the GSDML file was written
> from the block below, byte for byte. Import into TIA / CODESYS still open (step 10b.4).

## Goal
Give the process-image service its own PROFINET device description, "ProInspect SoftFieldbus" (DeviceID 0x0002).
It has exactly one 256-byte module in each direction, already inserted when the engineer adds the device. The
generic SoftPN GSDML (DeviceID 0x0001, sizes 16..256) stays for `pn_device_demo` and the tests. The module ident
numbers are the existing `IN256` / `OUT256` ones, so softpn needs no change.

## Documentation References
- [Phase 9.09 - device model and GSDML](../profinet/Phase9.09-ModelAndGsdml.md)
- [softpn-siemens-setup.md, 4.1](../../softpn-siemens-setup.md#41-install-the-gsdml): the TIA fixes this file must keep
- [PI GSDML specification and Checker](https://www.profibus.com/download/gsdml-specification-for-profinet)

---

## Step 10b.1: The GSDML file

**File:** `gsdml/GSDML-V2.43-Univision-ProInspect-20261021.xml`

### Purpose
This is a copy of `GSDML-V2.43-Univision-SoftPN-20261013.xml`, with all the fixes TIA needed (MinDeviceInterval,
full ReductionRatio list, PTP / DCP boundary, NameOfStationNotTransferable), reduced to the two modules the service
uses. `FixedInSlots` makes CODESYS and TIA insert both modules when the device is added, so the engineer cannot
pick a wrong size.

### Implementation

```xml
<?xml version="1.0" encoding="utf-8"?>
<!--
  GSDML for ProInspectProcessImageSoftFieldbusService with Transport=profinet (Univision softpn),
  Conformance Class A, RT_CLASS_1.

  - VendorID 0xFFFF is a PLACEHOLDER (as in GSDML-V2.43-Univision-SoftPN-*.xml). Replace it, together with the
    service's Parameters\profinet\VendorId, with the PI vendor ID assigned to Univision before any use outside the lab.
  - DeviceID 0x0002 = service Parameters\profinet\DeviceId (default 2). The generic SoftPN device is 0x0001.
  - Module ident numbers = src/pn/pn_model.cpp (256-byte variants); pn_selftest checks this file.
  - Validate with the PI GSDML Checker after every change.

  Slot 1: "Input 256 bytes (PC to PLC)"  - bytes 0..251 = HilscherProcessImageOutputData, 252..255 = 0
  Slot 2: "Output 256 bytes (PLC to PC)" - bytes 0..111 = HilscherProcessImageInputData, 112..255 unused
  Byte order of WORD / DWORD / REAL: the service's Parameters\profinet\ByteOrder (default big = Siemens).
-->
<ISO15745Profile xmlns="http://www.profibus.com/GSDML/2003/11/DeviceProfile"
                 xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
                 xsi:schemaLocation="http://www.profibus.com/GSDML/2003/11/DeviceProfile ..\xsd\GSDML-DeviceProfile-V2.43.xsd">
  <ProfileHeader>
    <ProfileIdentification>PROFINET Device Profile</ProfileIdentification>
    <ProfileRevision>1.00</ProfileRevision>
    <ProfileName>Device Profile for PROFINET Devices</ProfileName>
    <ProfileSource>PROFIBUS Nutzerorganisation e. V. (PNO)</ProfileSource>
    <ProfileClassID>Device</ProfileClassID>
    <ISO15745Reference>
      <ISO15745Part>4</ISO15745Part>
      <ISO15745Edition>1</ISO15745Edition>
      <ProfileTechnology>GSDML</ProfileTechnology>
    </ISO15745Reference>
  </ProfileHeader>
  <ProfileBody>
    <DeviceIdentity VendorID="0xFFFF" DeviceID="0x0002">
      <InfoText TextId="T_DeviceInfo"/>
      <VendorName Value="Univision"/>
    </DeviceIdentity>
    <DeviceFunction>
      <Family MainFamily="Ident Systems" ProductFamily="ProInspect"/>
    </DeviceFunction>
    <ApplicationProcess>
      <DeviceAccessPointList>
        <DeviceAccessPointItem ID="DAP1" PhysicalSlots="0..2" ModuleIdentNumber="0x00000001"
                               MinDeviceInterval="256" ImplementationType="Software"
                               DNS_CompatibleName="proinspect" FixedInSlots="0" ObjectUUID_LocalIndex="1"
                               RequiredSchemaVersion="V2.4" MultipleWriteSupported="true"
                               PNIO_Version="V2.43" DeviceAccessSupported="false"
                               CheckDeviceID_Allowed="true" LLDP_NoD_Supported="true" ResetToFactoryModes="2"
                               NameOfStationNotTransferable="false">
          <ModuleInfo>
            <Name TextId="T_DAP_Name"/>
            <InfoText TextId="T_DAP_Info"/>
            <VendorName Value="Univision"/>
            <OrderNumber Value="PROINSPECT-SFB-PN"/>
            <HardwareRelease Value="1"/>
            <SoftwareRelease Value="V1.0.0"/>
          </ModuleInfo>
          <CertificationInfo ConformanceClass="A" ApplicationClass="" NetloadClass="I"/>
          <IOConfigData MaxInputLength="1440" MaxOutputLength="1440"/>
          <UseableModules>
            <!-- FixedInSlots: inserted automatically when the device is added, cannot be removed or replaced -->
            <ModuleItemRef ModuleItemTarget="IN256" AllowedInSlots="1" FixedInSlots="1"/>
            <ModuleItemRef ModuleItemTarget="OUT256" AllowedInSlots="2" FixedInSlots="2"/>
          </UseableModules>
          <VirtualSubmoduleList>
            <VirtualSubmoduleItem ID="DAP1_SUB" SubmoduleIdentNumber="0x00000001" FixedInSubslots="1"
                                  MayIssueProcessAlarm="false">
              <IOData/>
              <ModuleInfo>
                <Name TextId="T_DAP_Name"/>
                <InfoText TextId="T_DAP_Info"/>
              </ModuleInfo>
            </VirtualSubmoduleItem>
          </VirtualSubmoduleList>
          <SystemDefinedSubmoduleList>
            <InterfaceSubmoduleItem ID="IF1" SubslotNumber="32768" SubmoduleIdentNumber="0x00008000"
                                    TextId="T_Interface" SupportedRT_Classes="RT_CLASS_1"
                                    SupportedProtocols="LLDP" DCP_HelloSupported="false"
                                    PTP_BoundarySupported="true" DCP_BoundarySupported="true">
              <ApplicationRelations StartupMode="Advanced;Legacy">
                <!-- ReductionRatio: RT_CLASS_1 needs the full list 1..512 (TIA check 0x00010030_4), explicit
                     (check 0x00020020_16). Update times below 8 ms are excluded by MinDeviceInterval="256". -->
                <TimingProperties SendClock="32" ReductionRatio="1 2 4 8 16 32 64 128 256 512"/>
              </ApplicationRelations>
            </InterfaceSubmoduleItem>
            <PortSubmoduleItem ID="PORT1" SubslotNumber="32769" SubmoduleIdentNumber="0x00008001"
                               TextId="T_Port1" MaxPortRxDelay="350" MaxPortTxDelay="160">
              <MAUTypeList>
                <MAUTypeItem Value="16"/>
                <MAUTypeItem Value="30"/>
              </MAUTypeList>
            </PortSubmoduleItem>
          </SystemDefinedSubmoduleList>
        </DeviceAccessPointItem>
      </DeviceAccessPointList>
      <ModuleList>
      <ModuleItem ID="IN256" ModuleIdentNumber="0x00000014">
        <ModuleInfo>
          <Name TextId="T_IN256"/>
          <InfoText TextId="T_IN_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="IN256_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Input Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="256" TextId="T_IN_Data"/>
              </Input>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_IN256"/>
              <InfoText TextId="T_IN_Info"/>
            </ModuleInfo>
          </VirtualSubmoduleItem>
        </VirtualSubmoduleList>
      </ModuleItem>
      <ModuleItem ID="OUT256" ModuleIdentNumber="0x00000024">
        <ModuleInfo>
          <Name TextId="T_OUT256"/>
          <InfoText TextId="T_OUT_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="OUT256_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Output Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="256" TextId="T_OUT_Data"/>
              </Output>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_OUT256"/>
              <InfoText TextId="T_OUT_Info"/>
            </ModuleInfo>
          </VirtualSubmoduleItem>
        </VirtualSubmoduleList>
      </ModuleItem>
      </ModuleList>
      <ExternalTextList>
        <PrimaryLanguage>
          <Text TextId="T_DeviceInfo" Value="ProInspect process image over PROFINET (ProInspectProcessImageSoftFieldbusService, PC with a standard NIC)"/>
          <Text TextId="T_DAP_Name" Value="ProInspect SoftFieldbus"/>
          <Text TextId="T_DAP_Info" Value="ProInspect process image, update time 8 ms or more. Same layout as the EtherNet/IP assemblies 100 / 150"/>
          <Text TextId="T_Interface" Value="X1"/>
          <Text TextId="T_Port1" Value="X1 P1"/>
          <Text TextId="T_IN_Info" Value="PC to PLC: bytes 0..251 = process image (packetId, recipe, view status and results, measurements, outputs), 252..255 = 0"/>
          <Text TextId="T_OUT_Info" Value="PLC to PC: bytes 0..111 = process image (view control, recipe, recipeId, inputs, handled parts), 112..255 unused"/>
          <Text TextId="T_IN_Data" Value="Input data"/>
          <Text TextId="T_OUT_Data" Value="Output data"/>
          <Text TextId="T_IN256" Value="Input 256 bytes (PC to PLC)"/>
          <Text TextId="T_OUT256" Value="Output 256 bytes (PLC to PC)"/>
        </PrimaryLanguage>
      </ExternalTextList>
    </ApplicationProcess>
  </ProfileBody>
</ISO15745Profile>
```

### Key Concepts
1. **File name**: `GSDML-V<schema>-<vendor>-<device>-<yyyymmdd>.xml` is required by PI. When the file changes,
   change the date, so a tool that installed the old one sees a new file.
2. **Same module names as the SoftPN GSDML** ("Input 256 bytes (PC to PLC)"), so the CODESYS channel names and the
   TIA hardware identifiers follow the same pattern as in the echo tests.
3. **`DNS_CompatibleName="proinspect"`** = the service's default `StationName`, so a new device in CODESYS / TIA
   already has the right name.
4. **`MainFamily="Ident Systems"`**: the catalog folder where vision sensors usually are (TIA: *Other field devices ->
   PROFINET IO -> Ident Systems -> Univision -> ProInspect*).

---

## Step 10b.2: Self-test for the ProInspect GSDML

**File:** `tools/pn_selftest.cpp`

### Purpose
`testGsdml()` expects every module variant, so it cannot check this file. A second check makes sure that the
ProInspect GSDML has the 256-byte idents, DeviceID 2, the fixed slots, and none of the other sizes.

### Implementation

```cpp
// after testGsdml()
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

// main(): next to the existing --gsdml option
    std::string gsdmlProInspect = SOFTPN_PROINSPECT_GSDML;
    // in the argument loop
        else if (a == "--gsdml-proinspect" && i + 1 < argc)
            gsdmlProInspect = argv[++i];
    // after testGsdml(gsdml);
    testProInspectGsdml(gsdmlProInspect);

// usage comment at the top of the file
// Usage: pn_selftest [--gsdml path/to/GSDML.xml] [--gsdml-proinspect path/to/ProInspect GSDML.xml]
```

### Key Concepts
1. **17 new checks**: 10 for the module idents (2 present, 8 absent), 7 for length, DAP, interface, port, identity
   and the two fixed slots. The total goes from 112 to **129**.
2. **"skipped" is not a failure**, as with `--gsdml`: a test run from another folder still passes.

---

## Step 10b.3: CMake path of the new file

**File:** `CMakeLists.txt`, target `pn_selftest`

### Purpose
The self-test should find the file without arguments, as it does for the SoftPN GSDML.

### Implementation

```cmake
    add_executable(pn_selftest tools/pn_selftest.cpp)
    target_link_libraries(pn_selftest PRIVATE pn_controller)
    target_compile_definitions(pn_selftest PRIVATE
        SOFTPN_DEFAULT_GSDML="${CMAKE_CURRENT_SOURCE_DIR}/gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml"
        SOFTPN_PROINSPECT_GSDML="${CMAKE_CURRENT_SOURCE_DIR}/gsdml/GSDML-V2.43-Univision-ProInspect-20261021.xml")
```

### Key Concepts
1. **Rename together**: if the date in the file name changes, change it here too, or the check reports "skipped".

---

## Step 10b.4: Import check

### Purpose
The engineering tools are stricter than the self-test: TIA refused the first SoftPN GSDML four times.

### Procedure
- **TIA Portal**: Options -> Manage general station description files (GSD) -> install the file. Expected: no error,
  device under *Other field devices -> PROFINET IO -> Ident Systems -> Univision -> ProInspect -> ProInspect
  SoftFieldbus*. Drag it into the network view: the device view already shows slot 1 "Input 256 bytes (PC to PLC)"
  and slot 2 "Output 256 bytes (PLC to PC)", and no other module can be added.
- **CODESYS**: Tools -> Device Repository -> Install. Add it under a PROFINET IO Controller: both modules are already there.
- PI GSDML Checker: still an open item for both GSDML files.

---

## Verification Checklist

Results 2026-10-21, dev PC:

- [x] The file is well-formed XML (parsed with .NET `XmlDocument`); UTF-8 without BOM, LF, as the SoftPN GSDML
- [x] `pn_selftest`: **129/129 PASS**, section "ProInspect GSDML" not skipped
- [x] Negative check: `--gsdml-proinspect` pointed at the SoftPN GSDML -> 11 failures (8 sizes that must be absent,
      DeviceID, the two fixed slots), RESULT FAIL
- [x] `vs2026-analyze-debug`: 0 warnings
- [ ] TIA Portal installs the file; the device has both modules fixed (needs the Siemens test bed)
- [x] CODESYS installs the file; the device has both modules fixed (Control Win V3 x64 on 10.0.0.128, 2026-10-21:
      device "ProInspect_SoftFieldbus" under PN_Controller with Input_256_bytes_PC_to_PLC_ / Output_256_bytes_PLC_to_PC_
      inserted automatically)
- [x] The SoftPN GSDML is unchanged

---

## Next Phase
Continue to [Phase10c-CodesysProgram.md](Phase10c-CodesysProgram.md): CODESYS PLC program.
