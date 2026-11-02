// ue_wrap/actors/wall.cpp -- see ue_wrap/actors/wall.h.

#include "ue_wrap/actors/wall.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>
#include <cstring>

namespace ue_wrap::wall {
namespace {

namespace R  = reflection;
namespace SR = save_record;

constexpr const wchar_t* kWet = L"customWall_wetConcrete_C";

bool Verb(void* actor, const wchar_t* name) {
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(actor), name));
    return f.valid() && Call(actor, f);
}

bool Name(const std::wstring& s) {
    if (s.size() > 128) return false;
    for (wchar_t c : s)
        if (c < 32 || c > 126) return false;
    return true;
}
bool Empty(const std::wstring& s) { return s.empty() || s == L"None"; }

// A wet wall's drying: dryTime and timeToDry, which getData does not save. Appended to float group 0 after the
// native health and max health; a record of two floats (an older save) keeps BeginPlay's defaults.
bool DryValid(const std::vector<float>& f) {
    if (f.size() != 2 && f.size() != 4) return false;
    for (float v : f)
        if (!std::isfinite(v) || v < 0 || v > 1'000'000.f) return false;
    return f[1] > 0 && (f.size() == 2 || f[3] > 0);
}
bool DryOffsets(void* actor, int32_t& elapsed, int32_t& duration) {
    static R::InstanceOffset dryTime(L"dryTime"), timeToDry(L"timeToDry");
    elapsed = dryTime.Of(actor);
    duration = timeToDry.Of(actor);
    return elapsed >= 0 && duration >= 0 && elapsed != duration;
}
bool CaptureDrying(void* actor, SR::SaveRecord& record) {
    if (record.floats.size() != 1 || !DryValid(record.floats[0])) return false;
    int32_t e = -1, d = -1;
    if (!DryOffsets(actor, e, d)) return false;
    float elapsed = 0, duration = 0;
    std::memcpy(&elapsed, static_cast<uint8_t*>(actor) + e, sizeof(float));
    std::memcpy(&duration, static_cast<uint8_t*>(actor) + d, sizeof(float));
    if (!std::isfinite(elapsed) || elapsed < 0 || elapsed > 1'000'000.f || !std::isfinite(duration) || duration <= 0 ||
        duration > 1'000'000.f)
        return false;
    auto fields = record.floats[0];
    fields.resize(4);
    fields[2] = elapsed;
    fields[3] = duration;
    record.floats[0] = std::move(fields);
    return true;
}
bool RestoreDrying(void* actor, const SR::SaveRecord& record) {
    if (record.floats.size() != 1 || !DryValid(record.floats[0])) return false;
    if (record.floats[0].size() == 2) return true;
    int32_t e = -1, d = -1;
    if (!DryOffsets(actor, e, d)) return false;
    std::memcpy(static_cast<uint8_t*>(actor) + e, &record.floats[0][2], sizeof(float));
    std::memcpy(static_cast<uint8_t*>(actor) + d, &record.floats[0][3], sizeof(float));
    return true;
}

// loadData restores an outline's wallType but not the material row it selected: findMat sets it again.
bool RestoreMaterial(void* outline, const std::wstring& name) {
    if (KindOf(outline) != Kind::Outline) return false;
    if (Empty(name)) return true;  // no material chosen yet
    CachedObjRef ref;
    ref.Set(outline);
    const auto row = fname_utils::StringToFName(name);
    if (!ref.Get() || !row.ComparisonIndex) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(outline), L"findMat"));
    bool ok = false;
    return f.valid() && f.Set<R::FName>(L"B", row) && Call(outline, f) && ref.Get() &&
           f.GetRaw(L"return", &ok, sizeof(ok)) && ok;
}

void* Library() {
    static CachedObjRef library;
    if (library.Get()) return library.Get();
    void* cls = object_index::ClassByName(L"lib_C");
    if (!cls) return nullptr;
    object_index::ForEachInstance(
        cls,
        [](void* ctx, void* object, int32_t) {
            if (R::NameEquals(R::NameOf(object), L"Default__lib_C")) static_cast<CachedObjRef*>(ctx)->Set(object);
        },
        &library);
    return library.Get();
}

}  // namespace

Kind ClassKind(const std::wstring& name) {
    if (name == L"customWall_unfinished_C") return Kind::Outline;
    if (name == L"customWall_C" || name == L"customWall_rad_C" || name == kWet) return Kind::Wall;
    return Kind::None;
}

Kind KindOf(void* actor) {
    if (!actor || !R::IsLive(actor)) return Kind::None;
    void* cls = R::ClassOf(actor);
    if (!cls || !R::NameStartsWith(R::NameOf(cls), L"customWall")) return Kind::None;
    return ClassKind(R::ClassNameOf(actor));
}

bool IsWet(void* actor) { return actor && R::IsLive(actor) && R::ClassNameOf(actor) == kWet; }

bool IsPlacementCaller(Kind kind, void* function) {
    if (kind == Kind::None || !function) return false;
    void* owner = R::OuterOf(function);
    if (!owner) return false;
    const auto& fn = R::NameOf(function);
    const auto& cls = R::NameOf(owner);
    if (R::NameEquals(cls, L"prop_wallbuilder_C") && R::NameEquals(fn, L"addWall")) return true;
    return kind == Kind::Wall &&
           ((R::NameEquals(cls, L"customWall_unfinished_C") && R::NameEquals(fn, L"buildWall")) ||
            (R::NameEquals(cls, L"customWall_wetConcrete_C") &&
             R::NameEquals(fn, L"ExecuteUbergraph_customWall_wetConcrete")));
}

bool Capture(void* actor, SR::SaveRecord& record) {
    if (KindOf(actor) == Kind::None) return false;
    CachedObjRef ref;
    ref.Set(actor);
    const auto world = world_identity::Generation();
    if (!SR::CaptureRecordVia(actor, R::FindDispatchFunctionCached(R::ClassOf(actor), L"getData"), record) ||
        !ref.Get() || world != world_identity::Generation())
        return false;
    return !IsWet(actor) || CaptureDrying(actor, record);
}

bool Normalize(const SR::SaveRecord& in, SR::SaveRecord& out) {
    const Kind kind = ClassKind(in.className);
    if (kind == Kind::None || Empty(in.key) || !Name(in.key)) return false;
    for (float v : in.xform)
        if (!std::isfinite(v)) return false;
    float q = 0;
    for (int i = 0; i < 4; ++i) q += in.xform[i] * in.xform[i];
    if (q < .9f || q > 1.1f) return false;
    for (int i = 4; i < 7; ++i)
        if (std::fabs(in.xform[i]) > 2'000'000.f) return false;
    for (int i = 7; i < 10; ++i)
        if (in.xform[i] < .01f || in.xform[i] > 100.f) return false;
    const bool outline = kind == Kind::Outline;
    const size_t count = outline ? 4 : 2;
    if (in.ints.empty() || in.ints[0].size() != count || in.ints[0].back() != -1 || in.strings.empty() ||
        in.strings[0].size() != 1 || !Name(in.strings[0][0]))
        return false;
    const auto shape = in.ints[0][outline ? 2 : 0];
    if (shape < 0 || shape > 3) return false;
    SR::SaveRecord r;
    r.className = in.className;
    r.key = in.key;
    r.xform = in.xform;
    if (outline) {
        if (in.ints[0][0] < 1 || in.ints[0][0] > 1000000 || in.ints[0][1] < 0 || in.ints[0][1] > 1000000) return false;
    } else {
        // An empty material is what getData answers for a wall no builder finished (a spawned one): it is shared
        // as it is, since loadData gives it back the same.
        if (in.floats.empty()) return false;
        if (in.className == kWet) {
            if (!DryValid(in.floats[0])) return false;
        } else if (in.floats[0].size() != 2) {
            return false;
        }
        for (float f : in.floats[0])
            if (!std::isfinite(f) || f < 0 || f > 1000000.f) return false;
        if (in.floats[0][1] <= 0) return false;
        r.floats = {in.floats[0]};
    }
    r.ints = {in.ints[0]};
    r.strings = {in.strings[0]};
    out = std::move(r);
    return true;
}

std::wstring Key(void* actor) {
    if (KindOf(actor) == Kind::None) return {};
    static R::InstanceOffset field(L"key");
    const int32_t off = field.Of(actor);
    return off >= 0 ? SR::ReadFNameAt(actor, off) : std::wstring{};
}

bool SetKey(void* actor, const std::wstring& key) {
    if (KindOf(actor) == Kind::None || Empty(key) || !Name(key)) return false;
    static R::InstanceOffset field(L"key");
    const int32_t off = field.Of(actor);
    if (off < 0) return false;
    SR::WriteFNameField(static_cast<uint8_t*>(actor) + off, key);
    // Writing the key does not register it in mainGamemode.keyObj_*; getKey does.
    CachedObjRef ref;
    ref.Set(actor);
    return Verb(actor, L"getKey") && ref.Get() && SR::ReadFNameAt(actor, off) == key;
}

bool Apply(void* actor, const SR::SaveRecord& record) {
    if (KindOf(actor) != ClassKind(record.className) || R::ClassNameOf(actor) != record.className) return false;
    CachedObjRef ref;
    ref.Set(actor);
    // loadData calls init, which derives the brick count and collision from the scale: the scale goes first.
    if (!engine::SetActorScale3D(actor, {record.xform[7], record.xform[8], record.xform[9]}) || !ref.Get()) return false;
    if (!SR::ApplyRecordVia(actor, R::FindDispatchFunctionCached(R::ClassOf(actor), L"loadData"), record) || !ref.Get())
        return false;
    if (IsWet(actor) && !RestoreDrying(actor, record)) return false;
    if (ClassKind(record.className) == Kind::Outline && !RestoreMaterial(actor, record.strings[0][0])) return false;
    return ref.Get() && SetKey(actor, record.key);
}

void* Spawn(const SR::SaveRecord& record) {
    if (ClassKind(record.className) == Kind::None) return nullptr;
    void* cls = object_index::ClassByName(record.className.c_str());
    if (!cls) return nullptr;
    const FVector pos{record.xform[4], record.xform[5], record.xform[6]};
    const auto rot = engine::QuatToRotator(record.xform[0], record.xform[1], record.xform[2], record.xform[3]);
    void* a = engine::BeginDeferredSpawn(cls, pos, rot);
    if (!a) return nullptr;
    CachedObjRef ref;
    ref.Set(a);
    if (!engine::FinishDeferredSpawn(a, pos, rot) || !ref.Get() || !Apply(a, record)) {
        if (ref.Get()) engine::DestroyActor(a);
        return nullptr;
    }
    engine::SetActorScale3D(a, {record.xform[7], record.xform[8], record.xform[9]});
    return ref.Get();
}

bool Feed(void* wall, void* material, const std::function<bool()>& callerCurrent) {
    if (!callerCurrent || !callerCurrent()) return false;
    if (KindOf(wall) != Kind::Outline || !material || !R::IsLive(material)) return true;
    CachedObjRef wallRef, materialRef;
    wallRef.Set(wall);
    materialRef.Set(material);
    const auto world = world_identity::Generation();
    const auto current = [&] {
        return callerCurrent() && world == world_identity::Generation() && wallRef.Get() && materialRef.Get();
    };
    static R::InstanceOffset overlapField(L"overlap");
    const int32_t off = overlapField.Of(wall);
    if (off < 0 || !current()) return false;
    void* overlap = nullptr;
    std::memcpy(&overlap, static_cast<uint8_t*>(wall) + off, sizeof(overlap));
    if (!overlap || !R::IsLive(overlap)) return false;
    ParamFrame check(R::FindDispatchFunctionCached(R::ClassOf(overlap), L"IsOverlappingActor"));
    if (!check.valid() || !check.Set<void*>(L"Other", material) || !Call(overlap, check) || !current()) return false;
    bool touches = false;
    if (!check.GetRaw(L"ReturnValue", &touches, sizeof(touches))) return false;
    if (!touches || KindOf(wall) != Kind::Outline) return true;
    // The native graph reads only OtherActor: the rest of the event's arguments stay zero.
    ParamFrame feed(R::FindDispatchFunctionCached(R::ClassOf(wall), kOverlap));
    if (!feed.valid() || !feed.Set<void*>(L"OtherActor", material) || !current()) return false;
    return Call(wall, feed);
}

bool Damage(void* wall, float amount) {
    if (KindOf(wall) != Kind::Wall || !std::isfinite(amount) || amount <= 0) return false;
    ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(wall), L"damage"));
    FVector position{};
    if (!f.valid() || !engine::TryGetActorLocation(wall, position) || !f.Set<float>(L"damage", amount) ||
        !f.Set<FVector>(L"Location", position) || !f.Set<FRotator>(L"Rotation", {}))
        return false;
    return Call(wall, f);  // a destruction is the native outcome too
}

bool IsBuilderCall(void* tool, void* function) {
    if (!tool || !function || !R::IsLive(tool) || !R::IsLive(function) || !R::NameEquals(R::NameOf(function), L"addWall"))
        return false;
    void* owner = R::OuterOf(function);
    return owner && R::IsLive(owner) && R::NameEquals(R::NameOf(owner), L"prop_wallbuilder_C") &&
           R::IsDescendantOfAny(R::ClassOf(tool), &owner, 1);
}

bool CreativeAllowed(void* context, bool& allowed) {
    if (!context || !R::IsLive(context)) return false;
    void* library = Library();
    if (!library) return false;
    ParamFrame frame(R::FindDispatchFunctionCached(R::ClassOf(library), L"isCustom"));
    bool value = false;
    if (!frame.valid() || !frame.Set<void*>(L"__WorldContext", context) || !Call(library, frame) ||
        !frame.GetRaw(L"isCustom", &value, sizeof(value)))
        return false;
    allowed = value;
    return true;
}

bool FinishCreative(void* outline) {
    if (KindOf(outline) != Kind::Outline) return false;
    ParamFrame frame(R::FindDispatchFunctionCached(R::ClassOf(outline), L"buildWall"));
    return frame.valid() && frame.Set<bool>(L"isCheat", true) && Call(outline, frame);
}

}  // namespace ue_wrap::wall
