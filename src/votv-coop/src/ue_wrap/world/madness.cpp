// ue_wrap/world/madness.cpp -- see ue_wrap/world/madness.h.
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/ue_wrap/world/madness.cpp (MIT).

#include "ue_wrap/world/madness.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/engine/engine_widget.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::madness {
namespace {

namespace R = reflection;

constexpr const wchar_t* kMasterClass = L"madnessCombatMaster_C";
constexpr const wchar_t* kWidgetClass = L"ui_madnessCombat_C";
constexpr const wchar_t* kMusic = L"SoundWave /Game/audio/music/MC_music.MC_music";
constexpr float kMusicVolume = 0.5f;

// A live object in the pointer property `prop` of `obj`, or null.
void* ObjectField(void* obj, const wchar_t* prop) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), prop);
    if (off < 0) return nullptr;
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

bool Alive(void* obj, int32_t idx) { return obj && idx >= 0 && R::IsLiveByIndex(obj, idx); }

// A parameterless function's return, called on `obj` and found on its class (or the class it is declared on).
template <class T>
bool CallReturning(void* obj, const wchar_t* fnName, T& out) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || f.ParamOffset(L"ReturnValue") < 0 || !Call(obj, f)) return false;
    return f.GetRaw(L"ReturnValue", &out, static_cast<int32_t>(sizeof(T)));
}

// UGameplayStatics::SpawnSound2D(MC_music) from `startTime`: the component it returns, or null.
void* StartMusic(float startTime) {
    static void* sGsCdo = nullptr;
    static void* sSpawnFn = nullptr;
    if (!sGsCdo) sGsCdo = R::FindClassDefaultObject(L"GameplayStatics");
    if (!sSpawnFn && sGsCdo) sSpawnFn = R::FindFunction(R::ClassOf(sGsCdo), L"SpawnSound2D");
    void* sound = engine::FindSound(kMusic);
    void* context = engine::GetWorldContext();
    if (!sGsCdo || !sSpawnFn || !sound || !context) return nullptr;
    ParamFrame f(sSpawnFn);
    if (!f.valid()) return nullptr;
    f.Set<void*>(L"WorldContextObject", context);
    f.Set<void*>(L"Sound", sound);
    f.Set<float>(L"VolumeMultiplier", kMusicVolume);
    f.Set<float>(L"PitchMultiplier", 1.f);
    f.Set<float>(L"StartTime", startTime);
    f.Set<void*>(L"ConcurrencySettings", nullptr);
    f.Set<bool>(L"bPersistAcrossLevelTransition", false);
    f.Set<bool>(L"bAutoDestroy", true);
    if (!Call(sGsCdo, f)) return nullptr;
    void* made = f.Get<void*>(L"ReturnValue");
    return (made && R::IsLive(made)) ? made : nullptr;
}

// WidgetBlueprintLibrary::Create(ui_madnessCombat_C), put on the screen: the widget, or null.
void* MakeWidget() {
    static void* sWblCdo = nullptr;
    static void* sCreateFn = nullptr;
    if (!sWblCdo) sWblCdo = R::FindClassDefaultObject(L"WidgetBlueprintLibrary");
    if (!sCreateFn && sWblCdo) sCreateFn = R::FindFunction(R::ClassOf(sWblCdo), L"Create");
    void* type = object_index::ClassByName(kWidgetClass);
    void* context = engine::GetWorldContext();
    if (!sWblCdo || !sCreateFn || !type || !context) return nullptr;
    ParamFrame f(sCreateFn);
    void* noPlayer = nullptr;
    if (!f.valid() || !f.Set(L"WorldContextObject", context) || !f.Set(L"WidgetType", type) ||
        !f.Set(L"OwningPlayer", noPlayer) || !Call(sWblCdo, f))
        return nullptr;
    void* made = f.Get<void*>(L"ReturnValue");
    if (!made || !R::IsLive(made) || R::ClassOf(made) != type) return nullptr;
    if (!engine::AddWidgetToViewport(made, 0)) {
        engine::RemoveWidgetFromViewport(made);
        return nullptr;
    }
    return made;
}

// The widget's count and its text, as the widget's own addCount writes them (without its spawn).
bool WriteKills(void* widget, int32_t kills) {
    const int32_t countOff = R::FindPropertyOffset(R::ClassOf(widget), L"count");
    void* label = ObjectField(widget, L"text_count");
    if (countOff < 0 || !label) return false;
    const std::wstring text = L"KILLS: " + std::to_wstring(kills);
    if (!component_calls::SetText(label, text.c_str())) return false;
    std::memcpy(static_cast<uint8_t*>(widget) + countOff, &kills, sizeof(kills));
    return true;
}

void StopAudio(View& view) {
    if (Alive(view.audio, view.audioIdx)) component_calls::CallParamlessNamed(view.audio, L"Stop");
    view.audio = nullptr;
    view.audioIdx = -1;
}

void DropWidget(View& view) {
    if (Alive(view.widget, view.widgetIdx)) engine::RemoveWidgetFromViewport(view.widget);
    view.widget = nullptr;
    view.widgetIdx = -1;
    view.kills = -1;
}

}  // namespace

void* FindMaster() {
    void* cls = object_index::ClassByName(kMasterClass);
    void* world = world_identity::CurrentWorld();
    if (!cls || !world) return nullptr;
    struct Search {
        void* world;
        void* found;
    } search{world, nullptr};
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto* s = static_cast<Search*>(ctx);
        if (!s->found && R::IsLive(obj) && world_identity::WorldOf(obj) == s->world) s->found = obj;
    }, &search);
    return search.found;
}

bool Read(void* master, State& out) {
    if (!master || !R::IsLive(master)) return false;
    State st{};
    st.active = true;
    if (void* hud = ObjectField(master, L"killcount")) {
        const int32_t countOff = R::FindPropertyOffset(R::ClassOf(hud), L"count");
        if (countOff < 0) return false;
        std::memcpy(&st.kills, static_cast<const uint8_t*>(hud) + countOff, sizeof(st.kills));
        if (st.kills < 0) st.kills = 0;
        st.hud = true;
    }
    if (void* audio = ObjectField(master, L"MC_music")) {
        bool playing = false;
        if (!CallReturning(audio, L"IsPlaying", playing)) return false;
        st.playing = playing;
    }
    float age = 0.f;
    if (!CallReturning(master, L"GetGameTimeSinceCreation", age) || !std::isfinite(age) || age < 0.f) return false;
    st.elapsed = age;
    out = st;
    return true;
}

void Clear(View& view) {
    StopAudio(view);
    DropWidget(view);
    view.started = false;
    view.world = 0;
}

bool Apply(View& view, const State& state, float startTime) {
    const uint32_t gen = world_identity::Generation();
    if (!state.active || !world_identity::CurrentWorld()) {
        Clear(view);
        return !state.active;
    }
    if (view.world != gen) {
        // What was made in a world since left: the music component went with that world; the widget may have
        // outlived it on the game instance, so it comes off the screen.
        Clear(view);
        view.world = gen;
    }
    bool ok = true;
    if (state.playing) {
        if (!view.started) {
            void* audio = StartMusic(std::isfinite(startTime) && startTime > 0.f ? startTime : 0.f);
            if (audio) {
                view.audio = audio;
                view.audioIdx = R::InternalIndexOf(audio);
                view.started = true;
            } else {
                ok = false;
            }
        }
    } else {
        StopAudio(view);
        view.started = false;
    }
    if (state.hud) {
        if (!Alive(view.widget, view.widgetIdx)) {
            view.widget = MakeWidget();
            view.widgetIdx = view.widget ? R::InternalIndexOf(view.widget) : -1;
            view.kills = -1;
        }
        if (!view.widget) {
            ok = false;
        } else if (view.kills != state.kills) {
            if (WriteKills(view.widget, state.kills)) view.kills = state.kills;
            else ok = false;
        }
    } else {
        DropWidget(view);
    }
    return ok;
}

}  // namespace ue_wrap::madness
