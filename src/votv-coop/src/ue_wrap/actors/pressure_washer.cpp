// ue_wrap/actors/pressure_washer.cpp -- see ue_wrap/actors/pressure_washer.h. Ported from the
// WatchersOfTheVoid fork (MIT).

#include "ue_wrap/actors/pressure_washer.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ue_wrap::pressure_washer {
namespace {

namespace R = reflection;

// The two members' places, read off the washer's class and re-read when the class object changes (a
// Blueprint class is a new object in every world).
CachedObjRef g_class;
int32_t g_level = -1, g_fire = -1;
uint8_t g_fireMask = 0;

bool Fields(void* actor) {
    if (!IsWasher(actor)) return false;
    void* cls = R::ClassOf(actor);
    if (!g_class.Is(cls) || g_level < 0 || g_fire < 0 || !g_fireMask) {
        g_class.Set(cls);
        g_level = R::FindPropertyOffset(cls, L"level");
        g_fire = -1;
        g_fireMask = 0;
        R::FindBoolProperty(cls, L"fire", g_fire, g_fireMask);
    }
    return g_level >= 0 && g_fire >= 0 && g_fireMask;
}

// The spray's three assets, found by name in their packages once loaded; a miss is asked again at most
// once a second.
void* Asset(size_t index) {
    struct Row { const wchar_t* name; const wchar_t* cls; const wchar_t* path; CachedObjRef object; uint64_t retry = 0; };
    static Row rows[]{
        {L"eff_waterThrower", L"ParticleSystem", L"/Game/particles/eff_waterThrower"},
        {L"wade_Cue", L"SoundCue", L"/Game/audio/effects/physics/footsteps/wade_Cue"},
        {L"att_default", L"SoundAttenuation", L"/Game/audio/misc/att_default"}};
    Row& row = rows[index];
    if (void* live = row.object.Get()) return live;
    const auto now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now < row.retry) return nullptr;
    row.retry = now + 1000;
    if (void* cls = object_index::ClassByName(row.cls)) {
        object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            Row& r = *static_cast<Row*>(ctx);
            if (!R::NameEquals(R::NameOf(obj), r.name)) return;
            void* package = R::OuterOf(obj);
            if (package && !R::OuterOf(package) && R::NameEquals(R::NameOf(package), r.path)) r.object.Set(obj);
        }, &row);
    }
    return row.object.Get();
}

}  // namespace

bool IsWasher(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* cls = R::ClassOf(actor);
    for (int hops = 0; hops < 16 && cls; ++hops, cls = R::SuperStructOf(cls))
        if (R::NameEquals(R::NameOf(cls), L"prop_waterShooter_C")) return true;
    return false;
}

bool Read(void* actor, bool& firing, float& level) {
    if (!Fields(actor)) return false;
    std::memcpy(&level, static_cast<uint8_t*>(actor) + g_level, sizeof(level));
    firing = (static_cast<uint8_t*>(actor)[g_fire] & g_fireMask) != 0;
    return std::isfinite(level);
}

bool SetRemoteLevel(void* actor, float level) {
    if (!std::isfinite(level) || level < 0.f || level > 10.f || !Fields(actor)) return false;
    if (!engine::SetActorTickEnabled(actor, false) || !R::IsLive(actor)) return false;
    std::memcpy(static_cast<uint8_t*>(actor) + g_level, &level, sizeof(level));
    static_cast<uint8_t*>(actor)[g_fire] &= uint8_t(~g_fireMask);
    return true;
}

bool EmitRemote(void* actor, const FVector& origin, const FRotator& aim) {
    if (!IsWasher(actor)) return false;
    for (float value : {origin.X, origin.Y, origin.Z, aim.Pitch, aim.Yaw, aim.Roll})
        if (!std::isfinite(value)) return false;
    void* effect = Asset(0);
    void* cdo = R::FindClassDefaultObject(L"GameplayStatics");
    if (!cdo || !effect) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(cdo), L"SpawnEmitterAtLocation"));
    if (!f.valid() || !f.Set<void*>(L"WorldContextObject", actor) || !f.Set<void*>(L"EmitterTemplate", effect) ||
        !f.Set(L"Location", origin) || !f.Set(L"Rotation", aim) || !f.Set(L"Scale", FVector{1, 1, 1}) ||
        !f.Set(L"bAutoDestroy", true) || !f.Set<uint8_t>(L"PoolingMethod", 0) || !f.Set(L"bAutoActivate", true) ||
        !Call(cdo, f) || !f.Get<void*>(L"ReturnValue"))
        return false;
    void* sound = Asset(1);
    void* attenuation = Asset(2);
    if (sound && attenuation && R::IsLive(actor))
        engine::PlaySoundAtLocation(actor, sound, origin, attenuation, 1.f, 1.5f);
    return true;
}

}  // namespace ue_wrap::pressure_washer
