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
