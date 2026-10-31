// coop/props/prop_food_state.cpp -- see header.

#include "coop/props/prop_food_state.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"   // TryGetWorldTimeSeconds: the receiver's own clock

#include <cmath>
#include <cstring>
#include <vector>

namespace coop::props::prop_food_state {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

// Aprop_food_C::getData's floats group 0: [lifespan, temperature, ripeness, GetTimeSeconds]. The
// layout is the one coop/dev/food_clock_probe reads; a subclass that calls its parent first keeps it.
constexpr size_t kStampGroup = 0;
constexpr size_t kStampIndex = 3;

// The tag of OUR extra float group: {kCookTag, cookProgress}, last in the record. Exactly
// representable, never a value the game writes there, and stripped before any loadData sees it.
constexpr float kCookTag = -31337.25f;

ue_wrap::CachedObjRef g_foodClass;
ue_wrap::CachedObjRef g_cookingClass;

// A class by name through the object index (a hash probe, a miss included), held while it lives: a
// world reload can re-create a Blueprint class at a new address.
void* ClassNamed(ue_wrap::CachedObjRef& cache, const wchar_t* name) {
    if (void* cls = cache.Get()) return cls;
    void* cls = ue_wrap::object_index::ClassByName(name);
    cache.Set(cls);
    return cls;
}

bool IsA(void* actor, ue_wrap::CachedObjRef& cache, const wchar_t* name) {
    if (!actor || !R::IsLive(actor)) return false;
    void* base = ClassNamed(cache, name);
    void* cls = R::ClassOf(actor);
    return base && cls && (cls == base || R::IsDescendantOfAny(cls, &base, 1));
}

bool IsCookingFood(void* actor) { return IsA(actor, g_cookingClass, L"prop_cookingFood_C"); }

R::InstanceOffset g_cookOff{L"cookProgress"};

bool ValidProgress(float v) { return std::isfinite(v) && v >= 0.f && v <= 1000000.f; }

bool ReadCook(void* actor, float& out) {
    if (!IsCookingFood(actor)) return false;
    const int32_t off = g_cookOff.Of(actor);
    if (off < 0) return false;
    float v = 0.f;
    std::memcpy(&v, static_cast<const uint8_t*>(actor) + off, sizeof(v));
    if (!ValidProgress(v)) return false;
    out = v;
    return true;
}

bool IsCookGroup(const std::vector<float>& g) { return g.size() == 2 && g[0] == kCookTag; }

}  // namespace

bool IsFood(void* actor) { return IsA(actor, g_foodClass, L"prop_food_C"); }

void DecorateCapture(void* actor, SR::SaveRecord& rec) {
    if (!IsFood(actor)) return;
    float progress = 0.f;
    if (!ReadCook(actor, progress)) return;
    // A record that somehow already ends in our group (a re-publish of a parked copy) keeps one.
    if (!rec.floats.empty() && IsCookGroup(rec.floats.back())) rec.floats.pop_back();
    rec.floats.push_back({kCookTag, progress});
}

bool PrepareApply(void* actor, SR::SaveRecord& rec, Carried& out) {
    out = Carried{};
    // The tag group comes out whatever the actor: no loadData may see it.
    if (!rec.floats.empty() && IsCookGroup(rec.floats.back())) {
        const float progress = rec.floats.back()[1];
        rec.floats.pop_back();
        if (ValidProgress(progress)) {
            out.haveCook = true;
            out.cookProgress = progress;
        }
    }
    if (!IsFood(actor)) return true;
    if (rec.floats.size() <= kStampGroup || rec.floats[kStampGroup].size() <= kStampIndex) return true;
    float& stamp = rec.floats[kStampGroup][kStampIndex];
    if (!std::isfinite(stamp) || stamp < 0.f) return true;   // not shaped like the food's clock: as sent
    float now = 0.f;
    if (!ue_wrap::engine::TryGetWorldTimeSeconds(now) || !std::isfinite(now)) {
        static int s_said = 0;
        if (s_said++ < 3)
            UE_LOGW("prop_food_state: this peer's world clock does not read -- a food record waits "
                    "rather than land with the sender's clock");
        return false;
    }
    // The sender's clock means nothing here. Rebased to ours, the catch-up loadData runs is zero,
    // and the food lands with the condition it had when its record was captured.
    stamp = now;
    return true;
}

void FinishApply(void* actor, const Carried& carried) {
    if (!carried.haveCook || !IsCookingFood(actor)) return;
    const int32_t off = g_cookOff.Of(actor);
    if (off < 0) return;
    // The field alone: no cookItem, no cooked() birth, nothing the native catch-up would run.
    std::memcpy(static_cast<uint8_t*>(actor) + off, &carried.cookProgress, sizeof(float));
}

}  // namespace coop::props::prop_food_state
