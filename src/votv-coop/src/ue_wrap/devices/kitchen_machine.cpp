// ue_wrap/devices/kitchen_machine.cpp -- see ue_wrap/devices/kitchen_machine.h.

#include "ue_wrap/devices/kitchen_machine.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_component.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ue_wrap::kitchen_machine {
namespace {

namespace R = reflection;

const wchar_t* const kClasses[] = {L"prop_microwave_C", L"prop_minioven_C", L"prop_bbq_C"};
CachedObjRef g_classes[3];

// One named property of an instance, its offset resolved on the instance's class and asked again when another
// class object comes (a world reload makes the Blueprint class anew). Each machine keeps its own, so a field is
// never re-resolved back and forth between two classes.
struct Field {
    const wchar_t* name;
    CachedObjRef cls;
    int32_t off = -1;
    explicit Field(const wchar_t* n) : name(n) {}
    int32_t Of(void* a) {
        void* c = a ? R::ClassOf(a) : nullptr;
        if (!c) return -1;
        if (!cls.Is(c)) {
            cls.Set(c);
            off = R::FindPropertyOffset(c, name);
        }
        return off;
    }
    template <class T> bool Get(void* a, T& v) {
        const int32_t o = Of(a);
        if (o < 0) return false;
        std::memcpy(&v, static_cast<const uint8_t*>(a) + o, sizeof(T));
        return true;
    }
    template <class T> bool Put(void* a, const T& v) {
        const int32_t o = Of(a);
        if (o < 0) return false;
        std::memcpy(static_cast<uint8_t*>(a) + o, &v, sizeof(T));
        return true;
    }
};

// One Blueprint bool, its byte and mask, so a packed bool's neighbours are never written.
struct Bit {
    const wchar_t* name;
    CachedObjRef cls;
    int32_t off = -1;
    uint8_t mask = 0;
    explicit Bit(const wchar_t* n) : name(n) {}
    bool Resolve(void* a) {
        void* c = a ? R::ClassOf(a) : nullptr;
        if (!c) return false;
        if (!cls.Is(c)) {
            cls.Set(c);
            off = -1;
            mask = 0;
            if (!R::FindBoolProperty(c, name, off, mask)) { off = -1; mask = 0; }
        }
        return off >= 0 && mask;
    }
    bool Get(void* a, bool& v) {
        if (!Resolve(a)) return false;
        v = (static_cast<const uint8_t*>(a)[off] & mask) != 0;
        return true;
    }
    bool Put(void* a, bool v) {
        if (!Resolve(a)) return false;
        uint8_t& b = static_cast<uint8_t*>(a)[off];
        b = static_cast<uint8_t>(v ? (b | mask) : (b & ~mask));
        return true;
    }
};

// The microwave: its run and power, the opened bit its light reads, the time mode, time and timer, and the light,
// loop and plate it shows them with.
struct Microwave {
    Bit   active{L"active"}, powered{L"powered"}, opened{L"opened"};
    Field timeMode{L"timeMode"}, time{L"time"}, timer{L"timer"};
    Field light{L"PointLight"}, loop{L"microwave_loop"}, spin{L"spin"};
} g_mw;

// The mini oven: its run and power, mode and power level, and the two hover bits that say which control the
// player aims at.
struct MiniOven {
    Bit   active{L"active"}, powered{L"powered"}, inputSwitch{L"input_switch"}, inputTemp{L"input_temp"};
    Field mode{L"mode"}, power{L"power"};
} g_ov;

// The grill: its fire and lid, coals and burn, and the cooking loop with its volume.
struct Grill {
    Bit   active{L"active"}, lid{L"hasLid"};
    Field coals{L"coals"}, burn{L"a"}, loop{L"cookingLoop"};
    Field volume{L"VolumeMultiplier"};  // on the loop, an audio component
} g_gr;

void* Object(Field& f, void* a) {
    void* p = nullptr;
    return f.Get(a, p) && p && R::IsLive(p) ? p : nullptr;
}

// An audio component playing or not, as asked: Play from the start when it should and does not, Stop when it
// should not and does.
bool SetPlaying(void* audio, bool playing) {
    if (!audio) return false;
    const bool now = engine::IsAudioComponentPlaying(audio);
    if (now == playing) return true;
    return playing ? engine::PlayAudioComponent(audio) : component_calls::CallParamlessNamed(audio, L"Stop");
}

bool ReadFlag(Bit& b, void* a, uint8_t bit, uint8_t& flags) {
    bool v = false;
    if (!b.Get(a, v)) return false;
    if (v) flags |= bit;
    return true;
}

}  // namespace

Kind KindOf(void* actor) {
    void* cls = actor && R::IsLive(actor) ? R::ClassOf(actor) : nullptr;
    if (!cls) return Kind::None;
    for (int i = 0; i < 3; ++i) {
        void* base = g_classes[i].Get();
        if (!base) {
            base = object_index::ClassByName(kClasses[i]);
            if (base) g_classes[i].Set(base);
        }
        if (base && R::IsDescendantOfAny(cls, &base, 1)) return static_cast<Kind>(i + 1);
    }
    return Kind::None;
}

bool Valid(const State& s) {
    if (s.kind == Kind::None || static_cast<uint8_t>(s.kind) > 3) return false;
    if (!std::isfinite(s.remaining) || !std::isfinite(s.timer) || !std::isfinite(s.power) ||
        !std::isfinite(s.burn) || !std::isfinite(s.volume))
        return false;
    if (s.flags & ~(kActive | kPowered | kLid | kCooking)) return false;
    if (s.kind != Kind::Grill && (s.flags & (kLid | kCooking))) return false;
    if (s.mode < 0 || s.mode > 8) return false;
    if (s.remaining < 0.f || s.remaining > 3600.f || s.timer < 0.f || s.timer > 3600.f) return false;
    if (s.power < 0.f || s.power > 1000.f) return false;
    if (s.coals < 0 || s.coals > 16 || s.burn < 0.f || s.burn > 1.01f || s.volume < 0.f || s.volume > 1.f)
        return false;
    return true;
}

bool Read(void* actor, State& out) {
    State s{};
    s.kind = KindOf(actor);
    bool ok = true;
    switch (s.kind) {
    case Kind::Microwave:
        ok &= ReadFlag(g_mw.active, actor, kActive, s.flags);
        ok &= ReadFlag(g_mw.powered, actor, kPowered, s.flags);
        ok &= g_mw.timeMode.Get(actor, s.mode);
        ok &= g_mw.time.Get(actor, s.remaining);
        ok &= g_mw.timer.Get(actor, s.timer);
        break;
    case Kind::MiniOven:
        ok &= ReadFlag(g_ov.active, actor, kActive, s.flags);
        ok &= ReadFlag(g_ov.powered, actor, kPowered, s.flags);
        ok &= g_ov.mode.Get(actor, s.mode);
        ok &= g_ov.power.Get(actor, s.power);
        break;
    case Kind::Grill: {
        ok &= ReadFlag(g_gr.active, actor, kActive, s.flags);
        ok &= ReadFlag(g_gr.lid, actor, kLid, s.flags);
        ok &= g_gr.coals.Get(actor, s.coals);
        ok &= g_gr.burn.Get(actor, s.burn);
        void* loop = Object(g_gr.loop, actor);
        if (loop) {
            if (engine::IsAudioComponentPlaying(loop)) s.flags |= kCooking;
            g_gr.volume.Get(loop, s.volume);
        }
        s.volume = std::clamp(s.volume, 0.f, 1.f);
        break;
    }
    default:
        return false;
    }
    if (!ok || !Valid(s)) return false;
    out = s;
    return true;
}

bool Apply(void* actor, const State& s, const std::function<bool()>& current) {
    if (!Valid(s) || KindOf(actor) != s.kind) return false;
    CachedObjRef live;
    live.Set(actor);
    const auto valid = [&] { return live.Get() && (!current || current()); };
    State before{};
    if (!Read(actor, before) || !valid()) return false;
    switch (s.kind) {
    case Kind::Microwave: {
        g_mw.active.Put(actor, (s.flags & kActive) != 0);
        g_mw.powered.Put(actor, (s.flags & kPowered) != 0);
        g_mw.timeMode.Put(actor, s.mode);
        g_mw.time.Put(actor, s.remaining);
        g_mw.timer.Put(actor, s.timer);
        // The light shows a powered microwave that runs or stands open; the door's bit is this peer's own (the
        // door is swinger_pose_sync's), read, never written.
        bool opened = false;
        g_mw.opened.Get(actor, opened);
        const bool lit = (s.flags & kPowered) && ((s.flags & kActive) || opened);
        if (void* light = Object(g_mw.light, actor)) engine::SetComponentVisible(light, lit, false);
        if (!valid()) return false;
        const bool running = (s.flags & (kActive | kPowered)) == (kActive | kPowered);
        if (void* loop = Object(g_mw.loop, actor)) SetPlaying(loop, running);
        return valid();
    }
    case Kind::MiniOven:
        g_ov.active.Put(actor, (s.flags & kActive) != 0);
        g_ov.powered.Put(actor, (s.flags & kPowered) != 0);
        g_ov.mode.Put(actor, s.mode);
        g_ov.power.Put(actor, s.power);
        // upd repaints the switch, glow and sound from the fields; only a change of run or power needs it.
        if ((before.flags ^ s.flags) & (kActive | kPowered)) {
            if (!component_calls::CallParamlessNamed(actor, L"upd")) return false;
        }
        return valid();
    case Kind::Grill: {
        g_gr.active.Put(actor, (s.flags & kActive) != 0);
        g_gr.lid.Put(actor, (s.flags & kLid) != 0);
        g_gr.coals.Put(actor, s.coals);
        g_gr.burn.Put(actor, s.burn);
        // updLid shows or hides the lid; upd paints the fire and the coals' material. Neither burns fuel or heats:
        // ReceiveTick does, and on a client it is the host's.
        if ((before.flags ^ s.flags) & kLid) {
            if (!component_calls::CallParamlessNamed(actor, L"updLid") || !valid()) return false;
        }
        if (((before.flags ^ s.flags) & kActive) || before.coals != s.coals ||
            std::fabs(before.burn - s.burn) > 0.02f) {
            if (!component_calls::CallParamlessNamed(actor, L"upd") || !valid()) return false;
        }
        if (void* loop = Object(g_gr.loop, actor)) {
            component_calls::SetVolumeMultiplier(loop, s.volume);
            if (!valid()) return false;
            SetPlaying(loop, (s.flags & kCooking) != 0);
        }
        return valid();
    }
    default:
        return false;
    }
}

uint8_t ReadControl(void* actor) {
    if (KindOf(actor) != Kind::MiniOven) return 0;
    bool sw = false, temp = false;
    g_ov.inputSwitch.Get(actor, sw);
    g_ov.inputTemp.Get(actor, temp);
    return temp ? 2 : (sw ? 1 : 0);
}

bool Press(void* actor, void* player, uint8_t action, uint8_t control) {
    const Kind k = KindOf(actor);
    if (k != Kind::Microwave && k != Kind::MiniOven) return false;
    if (action != 4 && !(k == Kind::Microwave && action == 8)) return false;
    if (k == Kind::MiniOven && control != 1 && control != 2) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"actionOptionIndex");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !f.Set<uint8_t>(L"action", action)) return false;
    f.Set<void*>(L"player", player);
    if (k != Kind::MiniOven) return Call(actor, f);
    // The oven reads which control the press is for from its hover bits: the sender's, for this call.
    bool sw = false, temp = false;
    if (!g_ov.inputSwitch.Get(actor, sw) || !g_ov.inputTemp.Get(actor, temp)) return false;
    g_ov.inputSwitch.Put(actor, control == 1);
    g_ov.inputTemp.Put(actor, control == 2);
    CachedObjRef live;
    live.Set(actor);
    const bool ok = Call(actor, f);
    if (live.Get()) {
        g_ov.inputSwitch.Put(actor, sw);
        g_ov.inputTemp.Put(actor, temp);
    }
    return ok;
}

bool SetGrillFire(void* actor, bool burning) {
    State s{};
    if (!Read(actor, s) || s.kind != Kind::Grill) return false;
    if (burning) {
        if (s.flags & kActive) return true;
        if (s.coals <= 0) return false;
        return component_calls::CallParamlessNamed(actor, L"attemptIgnite");
    }
    if (!(s.flags & kActive)) return true;
    return g_gr.active.Put(actor, false) && component_calls::CallParamlessNamed(actor, L"upd");
}

bool RaiseGrillCoals(void* actor, int32_t coals) {
    if (KindOf(actor) != Kind::Grill || coals < 0 || coals > 16) return false;
    int32_t now = 0;
    if (!g_gr.coals.Get(actor, now)) return false;
    if (now >= coals) return true;
    return g_gr.coals.Put(actor, coals) && component_calls::CallParamlessNamed(actor, L"upd");
}

bool SetGrillLid(void* actor, bool on) {
    if (KindOf(actor) != Kind::Grill) return false;
    bool now = false;
    if (!g_gr.lid.Get(actor, now)) return false;
    if (now == on) return true;
    return g_gr.lid.Put(actor, on) && component_calls::CallParamlessNamed(actor, L"updLid");
}

void Animate(void* actor, float seconds) {
    if (KindOf(actor) != Kind::Microwave) return;
    bool active = false, powered = false;
    if (!g_mw.active.Get(actor, active) || !active || !g_mw.powered.Get(actor, powered) || !powered) return;
    void* spin = Object(g_mw.spin, actor);
    if (!spin) return;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(spin), L"K2_AddLocalRotation");
    if (!fn) return;
    ParamFrame f(fn);
    const FRotator rot{0.f, 60.f * std::clamp(seconds, 0.f, 0.1f), 0.f};
    if (f.valid() && f.Set<FRotator>(L"DeltaRotation", rot)) Call(spin, f);
}

}  // namespace ue_wrap::kitchen_machine
