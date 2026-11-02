// ue_wrap/actors/safe.cpp -- see ue_wrap/actors/safe.h.

#include "ue_wrap/actors/safe.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstdint>

namespace ue_wrap::safe {

namespace R = reflection;

bool IsSafe(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    int depth = 0;
    for (void* c = R::ClassOf(actor); c && depth++ < 64; c = R::SuperStructOf(c))
        if (R::NameEquals(R::NameOf(c), L"prop_safe_C")) return true;
    return false;
}

bool ReadLocked(void* actor, bool& locked) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!IsSafe(actor) || !R::FindBoolProperty(R::ClassOf(actor), L"locked", off, mask) || off < 0 || !mask)
        return false;
    locked = (static_cast<uint8_t*>(actor)[off] & mask) != 0;
    return true;
}

bool Unlock(void* actor) {
    bool locked = true;
    if (!ReadLocked(actor, locked)) return false;
    if (!locked) return true;  // a repeated completion must not create the contents again
    ParamFrame frame(R::FindDispatchFunctionCached(R::ClassOf(actor), L"hacked"));
    return frame.valid() && Call(actor, frame);
}

}  // namespace ue_wrap::safe
