# Phase 9.09: Device model and GSDML

> Part of the softpn PROFINET RT device PoC. Index: [README.md](README.md). Code blocks are the verified files from branch `feature/profinet-poc`.

## Goal
Define the slot layout, the module variants and their ident numbers once in code, write the GSDML the controller imports, and compare the controller's expected configuration with the real one.

## Documentation References
- [PI GSDML specification and GSDML Checker](https://www.profibus.com/download/gsdml-specification-for-profinet)

---

## Step 9.09.1: `pn_model.hpp`

**File:** `src/pn/pn_model.hpp`

### Purpose
Slot 0: DAP, interface (0x8000), port (0x8001). Slot 1: input module N bytes (PC -> PLC). Slot 2: output module N bytes (PLC -> PC). N = 16/32/64/128/256.

### Implementation

```cpp
// The device's real slot/submodule configuration and its comparison with the controller's
// expected configuration (ExpectedSubmoduleBlockReq -> ModuleDiffBlock).
//
//   slot 0  subslot 0x0001  DAP (device access point)          no IO
//   slot 0  subslot 0x8000  interface                           no IO
//   slot 0  subslot 0x8001  port 1                              no IO
//   slot 1  subslot 0x0001  input module  N bytes  (PC -> PLC)  "input" in PROFINET terms
//   slot 2  subslot 0x0001  output module N bytes  (PLC -> PC)  "output" in PROFINET terms
//
// The ident numbers below must match gsdml/GSDML-*-Univision-SoftPN-*.xml (pn_selftest checks it).
#pragma once

#include "pn_blocks.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace softpn {

constexpr uint32_t kDapModuleIdent = 0x00000001;
constexpr uint32_t kDapSubmoduleIdent = 0x00000001;
constexpr uint32_t kInterfaceSubmoduleIdent = 0x00008000;
constexpr uint32_t kPortSubmoduleIdent = 0x00008001;
constexpr uint32_t kIoSubmoduleIdent = 0x00000001;
constexpr uint16_t kSubslotDap = 0x0001;
constexpr uint16_t kSubslotInterface = 0x8000;
constexpr uint16_t kSubslotPort1 = 0x8001;
constexpr uint16_t kSlotInput = 1;
constexpr uint16_t kSlotOutput = 2;

struct ModuleVariant {
    size_t bytes;
    uint32_t inputModuleIdent;  // slot 1, data device -> controller
    uint32_t outputModuleIdent; // slot 2, data controller -> device
};

// 16/32/64/128/256 bytes -> input idents 0x10..0x14, output idents 0x20..0x24.
const std::vector<ModuleVariant>& moduleVariants();
const ModuleVariant* findModuleVariant(size_t bytes);

struct RealSubmodule {
    uint32_t api = 0;
    uint16_t slot = 0;
    uint16_t subslot = 0;
    uint32_t moduleIdent = 0;
    uint32_t submoduleIdent = 0;
    uint16_t inputLength = 0;  // bytes device -> controller
    uint16_t outputLength = 0; // bytes controller -> device
};

inline uint64_t submoduleKey(uint32_t api, uint16_t slot, uint16_t subslot)
{
    return (uint64_t(api) << 32) | (uint64_t(slot) << 16) | subslot;
}

class DeviceModel {
public:
    // inputSize = PC -> PLC (slot 1), outputSize = PLC -> PC (slot 2). Both must be a variant size.
    bool build(size_t inputSize, size_t outputSize, std::string* error = nullptr);
    const std::vector<RealSubmodule>& submodules() const { return subs_; }
    const RealSubmodule* find(uint32_t api, uint16_t slot, uint16_t subslot) const;

private:
    std::vector<RealSubmodule> subs_;
};

struct ConfigCheck {
    std::vector<DiffModule> diff;      // empty = expected configuration matches completely
    std::set<uint64_t> proper;         // submoduleKey() of every matching submodule
};

ConfigCheck compareExpected(const DeviceModel& model, const std::vector<ExpectedModule>& expected);

} // namespace softpn
```

### Key Concepts
1. **Ident numbers**: inputs 0x10-0x14, outputs 0x20-0x24, DAP 0x1 - must match the GSDML.
2. **`ConfigCheck.proper`**: only matching submodules get IOPS GOOD in the cyclic data.

---

## Step 9.09.2: `pn_model.cpp`

**File:** `src/pn/pn_model.cpp`

### Purpose
The variant table and `compareExpected()`, which produces the ModuleDiffBlock.

### Implementation

```cpp
#include "pn_model.hpp"

namespace softpn {

const std::vector<ModuleVariant>& moduleVariants()
{
    static const std::vector<ModuleVariant> variants = {
        {16, 0x00000010, 0x00000020},  {32, 0x00000011, 0x00000021},  {64, 0x00000012, 0x00000022},
        {128, 0x00000013, 0x00000023}, {256, 0x00000014, 0x00000024},
    };
    return variants;
}

const ModuleVariant* findModuleVariant(size_t bytes)
{
    for (const ModuleVariant& v : moduleVariants())
        if (v.bytes == bytes)
            return &v;
    return nullptr;
}

bool DeviceModel::build(size_t inputSize, size_t outputSize, std::string* error)
{
    const ModuleVariant* in = findModuleVariant(inputSize);
    const ModuleVariant* out = findModuleVariant(outputSize);
    if (!in || !out) {
        if (error)
            *error = "PROFINET input/output size must be 16, 32, 64, 128 or 256 bytes (GSDML module variants)";
        return false;
    }
    subs_.clear();
    subs_.push_back({0, 0, kSubslotDap, kDapModuleIdent, kDapSubmoduleIdent, 0, 0});
    subs_.push_back({0, 0, kSubslotInterface, kDapModuleIdent, kInterfaceSubmoduleIdent, 0, 0});
    subs_.push_back({0, 0, kSubslotPort1, kDapModuleIdent, kPortSubmoduleIdent, 0, 0});
    subs_.push_back({0, kSlotInput, 1, in->inputModuleIdent, kIoSubmoduleIdent, uint16_t(inputSize), 0});
    subs_.push_back({0, kSlotOutput, 1, out->outputModuleIdent, kIoSubmoduleIdent, 0, uint16_t(outputSize)});
    return true;
}

const RealSubmodule* DeviceModel::find(uint32_t api, uint16_t slot, uint16_t subslot) const
{
    for (const RealSubmodule& s : subs_)
        if (s.api == api && s.slot == slot && s.subslot == subslot)
            return &s;
    return nullptr;
}

namespace {

bool lengthsMatch(const ExpectedSubmodule& e, const RealSubmodule& r)
{
    uint16_t in = 0, out = 0;
    for (const DataDescription& d : e.data) {
        if (d.type == 1)
            in = d.length;
        else if (d.type == 2)
            out = d.length;
    }
    return in == r.inputLength && out == r.outputLength;
}

} // namespace

ConfigCheck compareExpected(const DeviceModel& model, const std::vector<ExpectedModule>& expected)
{
    ConfigCheck result;
    for (const ExpectedModule& em : expected) {
        const RealSubmodule* anyInSlot = nullptr;
        for (const RealSubmodule& r : model.submodules())
            if (r.api == em.api && r.slot == em.slot)
                anyInSlot = &r;

        DiffModule dm;
        dm.api = em.api;
        dm.slot = em.slot;
        if (!anyInSlot) {
            dm.ident = em.moduleIdent;
            dm.state = kModuleStateNoModule;
            result.diff.push_back(dm);
            continue;
        }
        dm.ident = anyInSlot->moduleIdent;
        const bool moduleOk = anyInSlot->moduleIdent == em.moduleIdent;
        dm.state = moduleOk ? kModuleStateProper : kModuleStateWrong;

        for (const ExpectedSubmodule& es : em.submodules) {
            const RealSubmodule* rs = model.find(em.api, em.slot, es.subslot);
            if (!rs) {
                dm.submodules.push_back({es.subslot, es.ident, kSubmoduleStateNoSubmodule});
                continue;
            }
            if (!moduleOk || rs->submoduleIdent != es.ident || !lengthsMatch(es, *rs)) {
                dm.submodules.push_back({es.subslot, rs->submoduleIdent, kSubmoduleStateWrong});
                continue;
            }
            result.proper.insert(submoduleKey(em.api, em.slot, es.subslot));
        }
        if (!moduleOk || !dm.submodules.empty())
            result.diff.push_back(std::move(dm));
    }
    return result;
}

} // namespace softpn
```

### Key Concepts
1. **Wrong length = wrong submodule**: a controller expecting 32 bytes where the device has 64 gets a ModuleDiff entry, not silently truncated data.

---

## Step 9.09.3: `GSDML-V2.43-Univision-SoftPN-20261013.xml`

**File:** `gsdml/GSDML-V2.43-Univision-SoftPN-20261013.xml`

### Purpose
The device description for CODESYS / TIA. One DAP, ten modules (5 input + 5 output sizes), RT_CLASS_1, send clock 1 ms, reduction ratio 8..512 (update time >= 8 ms).

> The file was generated from a small script so the ten module entries stay consistent; edit it by hand from now on.

### Implementation

```xml
<?xml version="1.0" encoding="utf-8"?>
<!--
  GSDML for the Univision software PROFINET device (softpn), Conformance Class A, RT_CLASS_1.

  PoC file:
  - VendorID 0xFFFF is a PLACEHOLDER. Replace it (and DeviceConfig::vendorId) with the PI vendor ID
    assigned to Univision before any use outside the lab.
  - The ident numbers must match src/pn/pn_model.cpp (pn_selftest checks them).
  - Validate with the PI GSDML Checker after every change.

  Slot 1: one "Input N bytes" module  (data PC -> PLC)
  Slot 2: one "Output N bytes" module (data PLC -> PC)
  N must equal DeviceConfig::inputSize / outputSize of the running device.
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
    <DeviceIdentity VendorID="0xFFFF" DeviceID="0x0001">
      <InfoText TextId="T_DeviceInfo"/>
      <VendorName Value="Univision"/>
    </DeviceIdentity>
    <DeviceFunction>
      <Family MainFamily="I/O" ProductFamily="SoftPN"/>
    </DeviceFunction>
    <ApplicationProcess>
      <DeviceAccessPointList>
        <DeviceAccessPointItem ID="DAP1" PhysicalSlots="0..2" ModuleIdentNumber="0x00000001"
                               MinDeviceInterval="256" ImplementationType="Software"
                               DNS_CompatibleName="softpn-device" FixedInSlots="0" ObjectUUID_LocalIndex="1"
                               RequiredSchemaVersion="V2.4" MultipleWriteSupported="true"
                               PNIO_Version="V2.43" DeviceAccessSupported="false"
                               CheckDeviceID_Allowed="true" LLDP_NoD_Supported="true" ResetToFactoryModes="2"
                               NameOfStationNotTransferable="false">
          <ModuleInfo>
            <Name TextId="T_DAP_Name"/>
            <InfoText TextId="T_DAP_Info"/>
            <VendorName Value="Univision"/>
            <OrderNumber Value="SOFTPN-1"/>
            <HardwareRelease Value="1"/>
            <SoftwareRelease Value="V0.1.0"/>
          </ModuleInfo>
          <CertificationInfo ConformanceClass="A" ApplicationClass="" NetloadClass="I"/>
          <IOConfigData MaxInputLength="1440" MaxOutputLength="1440"/>
          <UseableModules>
            <ModuleItemRef ModuleItemTarget="IN16" AllowedInSlots="1"/>
            <ModuleItemRef ModuleItemTarget="IN32" AllowedInSlots="1"/>
            <ModuleItemRef ModuleItemTarget="IN64" AllowedInSlots="1"/>
            <ModuleItemRef ModuleItemTarget="IN128" AllowedInSlots="1"/>
            <ModuleItemRef ModuleItemTarget="IN256" AllowedInSlots="1"/>
            <ModuleItemRef ModuleItemTarget="OUT16" AllowedInSlots="2"/>
            <ModuleItemRef ModuleItemTarget="OUT32" AllowedInSlots="2"/>
            <ModuleItemRef ModuleItemTarget="OUT64" AllowedInSlots="2"/>
            <ModuleItemRef ModuleItemTarget="OUT128" AllowedInSlots="2"/>
            <ModuleItemRef ModuleItemTarget="OUT256" AllowedInSlots="2"/>
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
                <!-- ReductionRatio: RT_CLASS_1 needs the full list 1..512 (TIA refuses a shortened list,
                     check 0x00010030_4) and it must be explicit (check 0x00020020_16: 32 x 8 = MinDeviceInterval).
                     Update times below 8 ms are excluded by MinDeviceInterval="256". -->
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
      <ModuleItem ID="IN16" ModuleIdentNumber="0x00000010">
        <ModuleInfo>
          <Name TextId="T_IN16"/>
          <InfoText TextId="T_IN_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="IN16_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Input Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="16" TextId="T_IN_Data"/>
              </Input>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_IN16"/>
              <InfoText TextId="T_IN_Info"/>
            </ModuleInfo>
          </VirtualSubmoduleItem>
        </VirtualSubmoduleList>
      </ModuleItem>
      <ModuleItem ID="IN32" ModuleIdentNumber="0x00000011">
        <ModuleInfo>
          <Name TextId="T_IN32"/>
          <InfoText TextId="T_IN_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="IN32_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Input Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="32" TextId="T_IN_Data"/>
              </Input>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_IN32"/>
              <InfoText TextId="T_IN_Info"/>
            </ModuleInfo>
          </VirtualSubmoduleItem>
        </VirtualSubmoduleList>
      </ModuleItem>
      <ModuleItem ID="IN64" ModuleIdentNumber="0x00000012">
        <ModuleInfo>
          <Name TextId="T_IN64"/>
          <InfoText TextId="T_IN_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="IN64_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Input Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="64" TextId="T_IN_Data"/>
              </Input>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_IN64"/>
              <InfoText TextId="T_IN_Info"/>
            </ModuleInfo>
          </VirtualSubmoduleItem>
        </VirtualSubmoduleList>
      </ModuleItem>
      <ModuleItem ID="IN128" ModuleIdentNumber="0x00000013">
        <ModuleInfo>
          <Name TextId="T_IN128"/>
          <InfoText TextId="T_IN_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="IN128_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Input Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="128" TextId="T_IN_Data"/>
              </Input>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_IN128"/>
              <InfoText TextId="T_IN_Info"/>
            </ModuleInfo>
          </VirtualSubmoduleItem>
        </VirtualSubmoduleList>
      </ModuleItem>
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
      <ModuleItem ID="OUT16" ModuleIdentNumber="0x00000020">
        <ModuleInfo>
          <Name TextId="T_OUT16"/>
          <InfoText TextId="T_OUT_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="OUT16_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Output Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="16" TextId="T_OUT_Data"/>
              </Output>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_OUT16"/>
              <InfoText TextId="T_OUT_Info"/>
            </ModuleInfo>
          </VirtualSubmoduleItem>
        </VirtualSubmoduleList>
      </ModuleItem>
      <ModuleItem ID="OUT32" ModuleIdentNumber="0x00000021">
        <ModuleInfo>
          <Name TextId="T_OUT32"/>
          <InfoText TextId="T_OUT_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="OUT32_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Output Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="32" TextId="T_OUT_Data"/>
              </Output>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_OUT32"/>
              <InfoText TextId="T_OUT_Info"/>
            </ModuleInfo>
          </VirtualSubmoduleItem>
        </VirtualSubmoduleList>
      </ModuleItem>
      <ModuleItem ID="OUT64" ModuleIdentNumber="0x00000022">
        <ModuleInfo>
          <Name TextId="T_OUT64"/>
          <InfoText TextId="T_OUT_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="OUT64_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Output Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="64" TextId="T_OUT_Data"/>
              </Output>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_OUT64"/>
              <InfoText TextId="T_OUT_Info"/>
            </ModuleInfo>
          </VirtualSubmoduleItem>
        </VirtualSubmoduleList>
      </ModuleItem>
      <ModuleItem ID="OUT128" ModuleIdentNumber="0x00000023">
        <ModuleInfo>
          <Name TextId="T_OUT128"/>
          <InfoText TextId="T_OUT_Info"/>
        </ModuleInfo>
        <VirtualSubmoduleList>
          <VirtualSubmoduleItem ID="OUT128_SUB" SubmoduleIdentNumber="0x00000001" MayIssueProcessAlarm="false">
            <IOData>
              <Output Consistency="All items consistency">
                <DataItem DataType="OctetString" Length="128" TextId="T_OUT_Data"/>
              </Output>
            </IOData>
            <ModuleInfo>
              <Name TextId="T_OUT128"/>
              <InfoText TextId="T_OUT_Info"/>
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
          <Text TextId="T_DeviceInfo" Value="Univision software PROFINET IO-Device (PC, standard NIC)"/>
          <Text TextId="T_DAP_Name" Value="SoftPN device"/>
          <Text TextId="T_DAP_Info" Value="Software PROFINET RT device, update time 8 ms or more"/>
          <Text TextId="T_Interface" Value="X1"/>
          <Text TextId="T_Port1" Value="X1 P1"/>
          <Text TextId="T_IN_Info" Value="Data from the PC to the PLC (slot 1)"/>
          <Text TextId="T_OUT_Info" Value="Data from the PLC to the PC (slot 2)"/>
          <Text TextId="T_IN_Data" Value="Input data"/>
          <Text TextId="T_OUT_Data" Value="Output data"/>
          <Text TextId="T_IN16" Value="Input 16 bytes (PC to PLC)"/>
          <Text TextId="T_IN32" Value="Input 32 bytes (PC to PLC)"/>
          <Text TextId="T_IN64" Value="Input 64 bytes (PC to PLC)"/>
          <Text TextId="T_IN128" Value="Input 128 bytes (PC to PLC)"/>
          <Text TextId="T_IN256" Value="Input 256 bytes (PC to PLC)"/>
          <Text TextId="T_OUT16" Value="Output 16 bytes (PLC to PC)"/>
          <Text TextId="T_OUT32" Value="Output 32 bytes (PLC to PC)"/>
          <Text TextId="T_OUT64" Value="Output 64 bytes (PLC to PC)"/>
          <Text TextId="T_OUT128" Value="Output 128 bytes (PLC to PC)"/>
          <Text TextId="T_OUT256" Value="Output 256 bytes (PLC to PC)"/>
        </PrimaryLanguage>
      </ExternalTextList>
    </ApplicationProcess>
  </ProfileBody>
</ISO15745Profile>
```

### Key Concepts
1. **VendorID 0xFFFF is a placeholder**: replace it (and `DeviceConfig::vendorId`) with the PI vendor ID before any use outside the lab.
2. **Validate** with the PI GSDML Checker after every change and note its warnings in the results page.
3. **`StartupMode="Advanced;Legacy"`**, `ResetToFactoryModes`, `MAUTypeList`: `[verify]` what CODESYS accepts.

---

## Step 9.09.4: Add the model

**File:** `CMakeLists.txt`

### Purpose
Append to the lists:

### Implementation

```cmake
src/pn/pn_model.cpp      # SOFTPN_SOURCES
src/pn/pn_model.hpp      # SOFTPN_HEADERS
```

### Key Concepts
1. **Self-check**: `pn_selftest` greps the GSDML for every ident number of the table.

---

## Verification Checklist

- [ ] `softpn` builds with 0 warnings
- [ ] The GSDML is well-formed XML (opens in a browser / `xmllint`)
- [ ] PI GSDML Checker run; warnings noted
- [ ] CODESYS: Tools -> Device Repository -> Install imports the file
- [ ] `pn_selftest` "GSDML ident numbers" passes

---

## Next Phase
Continue to [Phase9.10-CyclicEngine.md](Phase9.10-CyclicEngine.md): Cyclic RT_CLASS_1 data exchange.
