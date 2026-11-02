// ue_wrap/actors/torch.cpp -- see ue_wrap/actors/torch.h.

#include "ue_wrap/actors/torch.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ue_wrap::torch {
namespace {

namespace R = reflection;

// The two members' places, re-read when the class object changes (a Blueprint class is a new object in every world).
CachedObjRef g_class;
int32_t g_fuel = -1, g_burning = -1;
uint8_t g_burningMask = 0;

bool Fields(void* actor) {
    if (!IsTorch(actor)) return false;
    void* cls = R::ClassOf(actor);
    if (!g_class.Is(cls) || g_fuel < 0 || g_burning < 0 || !g_burningMask) {
        g_class.Set(cls);
        g_fuel = R::FindPropertyOffset(cls, L"fuel");
        g_burning = -1;
        g_burningMask = 0;
        R::FindBoolProperty(cls, L"burning", g_burning, g_burningMask);
    }
    return g_fuel >= 0 && g_burning >= 0 && g_burningMask;
}

bool Verb(void* actor, const wchar_t* name, const float* fuel) {
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(actor), name));
    if (!f.valid() || (fuel && !f.Set<float>(L"fuel", *fuel))) return false;
    return Call(actor, f);
}

}  // namespace

bool IsTorch(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* cls = R::ClassOf(actor);
    for (int hops = 0; hops < 16 && cls; ++hops, cls = R::SuperStructOf(cls))
        if (R::NameEquals(R::NameOf(cls), L"prop_torch_C")) return true;
    return false;
}

bool Read(void* actor, bool& burning, float& fuel) {
    if (!Fields(actor)) return false;
    std::memcpy(&fuel, static_cast<const uint8_t*>(actor) + g_fuel, sizeof(fuel));
    burning = (static_cast<const uint8_t*>(actor)[g_burning] & g_burningMask) != 0;
    return std::isfinite(fuel);
}

bool Ignite(void* actor, float fuel) {
    return std::isfinite(fuel) && fuel > 0.f && IsTorch(actor) && Verb(actor, L"ignite", &fuel);
}

bool Extinguish(void* actor) {
    return IsTorch(actor) && Verb(actor, L"extinguishFire", nullptr);
}

bool WriteFuel(void* actor, float fuel) {
    if (!std::isfinite(fuel) || fuel < 0.f || !Fields(actor)) return false;
    std::memcpy(static_cast<uint8_t*>(actor) + g_fuel, &fuel, sizeof(fuel));
    return true;
}

}  // namespace ue_wrap::torch
