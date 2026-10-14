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
