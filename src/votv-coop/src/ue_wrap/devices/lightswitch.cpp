// ue_wrap/devices/lightswitch.cpp -- see ue_wrap/devices/lightswitch.h. Engine access for VOTV
// light groups (Atrigger_lightRoot_C). Offsets resolved from the live class via
// reflection (version-portable); the Alpha 0.9.0-n values are logged fallbacks.

#include "ue_wrap/devices/lightswitch.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ue_wrap::lightswitch {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_rootCls    = nullptr;  // trigger_lightRoot_C UClass
int32_t g_keyOff     = -1;       // AtriggerBase_C::Key       (Alpha 0.9.0-n: 0x0260)
int32_t g_isActiveOff = -1;      // trigger_lightRoot_C::IsActive (0x02B8)
void*   g_runTriggerFn = nullptr;// runTrigger(owner, index) -- index 1/2 are the ABSOLUTE setters
void*   g_setActiveFn = nullptr; // setActive(bool) -- the group's breaker verb
int32_t g_breakerOff  = -1;      // trigger_lightRoot_C::active, the breaker (distinct from IsActive)

constexpr int32_t kKeyOffFallback      = 0x0260;
constexpr int32_t kIsActiveOffFallback = 0x02B8;

// --- The light SWITCH (Alightswitch_C) ---
std::atomic<bool> g_swResolved{false};
void*   g_swCls   = nullptr;  // lightswitch_C UClass
int32_t g_swKeyOff = -1;      // AtriggerBase_C::Key (shared base offset, 0x0260)
int32_t g_swAOff  = -1;       // Alightswitch_C::A (the flip bool, 0x02A0)
void*   g_useFn   = nullptr;  // use()
int32_t g_objectsOff = -1;    // triggerBase_C::objects (TArray<UObject*>): objects[0] is the switch's group
constexpr int32_t kSwitchAOffFallback = 0x02A0;

// --- The replayed switch's click (CallUseReplayed) ---
// use() ends in GameplayStatics::PlaySound2D, a UI click with no place: replayed for a remote
// peer's press it was heard at full volume anywhere on the map. While a replay runs, a pre hook on
// PlaySound2D (armed only for that synchronous call) cancels the 2D sound and records its cue; once
// use() returns, the cue plays at the switch through PlaySoundAtLocation with an attenuation built
// here. A local press keeps its native click. Adapted from ElvardFIC/WatchersOfTheVoid
// src/votv-coop/src/ue_wrap/devices/lightswitch.cpp (MIT).
UE_FACT_FN(kRowReplayPlaySound2D, "lightswitch", L"GameplayStatics", L"PlaySound2D", Declared);

namespace sg = script_gate;
namespace UH = ufunction_hook;

void*   g_sound2DFn = nullptr;
bool    g_sound2DHooked = false;
bool    g_sound2DRefused = false;
int32_t g_offSound = -1, g_offVolume = -1, g_offPitch = -1;
void*   g_clickAttenuation = nullptr;  // rooted by SpawnSoundAttenuation, kept for the process

struct CapturedSound { void* sound; float volume; float pitch; };
constexpr int kMaxCaptured = 4;
CapturedSound g_captured[kMaxCaptured] = {};
int g_capturedCount = 0;
int g_replayDepth = 0;  // >0 while CallUseReplayed's use() runs; game-thread serial

sg::Verdict OnReplaySound2DPre(void*, void*, const uint8_t* parms) {
    if (g_replayDepth <= 0) return sg::Verdict::Run;
    // Inside the replay every 2D sound is the switch's: cancel it, whatever else happens, so a
    // failure below never falls back to a map-wide UI click.
    if (parms && g_capturedCount < kMaxCaptured) {
        CapturedSound c{};
        std::memcpy(&c.sound, parms + g_offSound, sizeof(c.sound));
        std::memcpy(&c.volume, parms + g_offVolume, sizeof(c.volume));
        std::memcpy(&c.pitch, parms + g_offPitch, sizeof(c.pitch));
        if (c.sound && std::isfinite(c.volume) && c.volume >= 0.f && std::isfinite(c.pitch) && c.pitch > 0.f)
            g_captured[g_capturedCount++] = c;
    }
    return sg::Verdict::Cancel;
}

// The hook, installed disarmed on first need. False when it cannot be (refused for the process).
bool EnsureReplaySoundHook() {
    if (g_sound2DHooked) return true;
    if (g_sound2DRefused) return false;
    const facts::Resolved fn = facts::Resolve(kRowReplayPlaySound2D);
    if (!fn) return false;  // not resolvable yet: the next replay asks again
    g_sound2DFn = fn.Fn();
    g_offSound  = R::FindParamOffset(g_sound2DFn, L"Sound");
    g_offVolume = R::FindParamOffset(g_sound2DFn, L"VolumeMultiplier");
    g_offPitch  = R::FindParamOffset(g_sound2DFn, L"PitchMultiplier");
    if (g_offSound < 0 || g_offVolume < 0 || g_offPitch < 0 ||
        !UH::InstallPreHook(fn, &OnReplaySound2DPre, /*armed=*/false)) {
        g_sound2DRefused = true;
        UE_LOGE("lightswitch: no pre hook on PlaySound2D (offsets %d/%d/%d) -- a replayed switch keeps "
                "its map-wide 2D click", g_offSound, g_offVolume, g_offPitch);
        return false;
    }
    g_sound2DHooked = true;
    UE_LOGI("lightswitch: replayed switch clicks are placed at the switch (PlaySound2D pre hook)");
    return true;
}

void* ClickAttenuation() {
    if (g_clickAttenuation) return g_clickAttenuation;
    engine::SoundAttenuationConfig cfg{};
    cfg.shape = 0;              // sphere
    cfg.distanceAlgorithm = 0;  // linear: full volume to 1 m, silent by 15 m
    cfg.extents[0] = 100.f;
    cfg.falloffDistance = 1400.f;
    g_clickAttenuation = engine::SpawnSoundAttenuation(cfg);  // null: retried at the next replay
    return g_clickAttenuation;
}

struct ReplayScope {
    ReplayScope() {
        if (g_replayDepth++ == 0) {
            g_capturedCount = 0;
            UH::SetArmed(g_sound2DFn, &OnReplaySound2DPre, true);
        }
    }
    ~ReplayScope() {
        if (--g_replayDepth == 0) UH::SetArmed(g_sound2DFn, &OnReplaySound2DPre, false);
    }
};

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* rootCls = R::FindClass(L"trigger_lightRoot_C");
    if (!rootCls) return false;

    // Key is declared on the trigger base, which the property lookup climbs to.
    int32_t keyOff = R::FindPropertyOffset(rootCls, L"Key");
    if (keyOff < 0) {
        UE_LOGW("lightswitch: reflected Key offset not found -- using fallback 0x%04X", kKeyOffFallback);
        keyOff = kKeyOffFallback;
    }
    int32_t isActiveOff = R::FindPropertyOffset(rootCls, L"IsActive");
    if (isActiveOff < 0) {
        UE_LOGW("lightswitch: reflected IsActive offset not found -- using fallback 0x%04X", kIsActiveOffFallback);
        isActiveOff = kIsActiveOffFallback;
    }
    void* setActiveFn = R::FindFunction(rootCls, L"SetActive");
    if (!setActiveFn) {
        UE_LOGW("lightswitch: SetActive UFunction not found -- not ready");
        return false;
    }
    // `active` (the breaker) and `IsActive` (the live state) are two separate FBoolProperties, and
    // FName lookup ignores case, so these two names resolve two fields. Read only: the breaker is
    // set through the group's own setActive.
    g_breakerOff  = R::FindPropertyOffset(rootCls, L"Active");
    g_setActiveFn = setActiveFn;
    void* runTriggerFn = R::FindFunction(rootCls, L"runTrigger");
    if (!runTriggerFn) UE_LOGW("lightswitch: runTrigger UFunction not found -- absolute group apply disabled");
    g_runTriggerFn = runTriggerFn;

    g_rootCls     = rootCls;
    g_keyOff      = keyOff;
    g_isActiveOff = isActiveOff;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("lightswitch: resolved trigger_lightRoot_C=%p Key@0x%04X IsActive@0x%04X SetActive=%p",
            rootCls, keyOff, isActiveOff, setActiveFn);
    return true;
}


bool IsLightRoot(void* obj) {
    if (!obj || !g_rootCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_rootCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* root) {
    if (!root || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(root) + g_keyOff);
    return R::ToString(key);
}

bool TryReadActive(void* root, bool& on) {
    if (!root || g_isActiveOff < 0) return false;
    on = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(root) + g_isActiveOff);
    return true;
}

// --- The light SWITCH (Alightswitch_C) ---

bool EnsureSwitchResolved() {
    if (g_swResolved.load(std::memory_order_acquire)) return true;
    void* cls = R::FindClass(L"lightswitch_C");
    if (!cls) return false;
    // Key and objects are declared on the trigger base, which the property lookup climbs to.
    int32_t keyOff = R::FindPropertyOffset(cls, L"Key");
    if (keyOff < 0) keyOff = kKeyOffFallback;
    int32_t aOff = R::FindPropertyOffset(cls, L"A");
    if (aOff < 0) {
        UE_LOGW("lightswitch: switch A offset not found -- using fallback 0x%04X", kSwitchAOffFallback);
        aOff = kSwitchAOffFallback;
    }
    void* useFn = R::FindFunction(cls, L"use");
    if (!useFn) { UE_LOGW("lightswitch: switch use() UFunction not found -- not ready"); return false; }
    g_objectsOff = R::FindPropertyOffset(cls, L"objects");
    g_swCls = cls; g_swKeyOff = keyOff; g_swAOff = aOff; g_useFn = useFn;
    g_swResolved.store(true, std::memory_order_release);
    UE_LOGI("lightswitch: resolved switch lightswitch_C=%p Key@0x%04X A@0x%04X objects@0x%04X use=%p",
            cls, keyOff, aOff, g_objectsOff, useFn);
    return true;
}

bool IsLightSwitch(void* obj) {
    if (!obj || !g_swCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_swCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetSwitchKeyString(void* sw) {
    if (!sw || g_swKeyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(sw) + g_swKeyOff);
    return R::ToString(key);
}

bool TryReadSwitchA(void* sw, bool& on) {
    if (!sw || g_swAOff < 0) return false;
    on = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(sw) + g_swAOff);
    return true;
}

bool CallUse(void* sw) {
    if (!sw || !g_useFn) return false;
    ParamFrame f(g_useFn);
    if (!f.valid()) return false;
    return Call(sw, f);
}

bool CallUseReplayed(void* sw) {
    if (!sw || !g_useFn) return false;
    if (!EnsureReplaySoundHook()) return CallUse(sw);  // no hook: the native (2D) click stays
    ParamFrame f(g_useFn);
    if (!f.valid()) return false;
    bool ran = false;
    int captured = 0;
    CapturedSound sounds[kMaxCaptured] = {};
    {
        ReplayScope scope;
        ran = Call(sw, f);
        if (g_replayDepth == 1) {  // the outermost replay owns what was captured
            captured = g_capturedCount;
            for (int i = 0; i < captured; ++i) sounds[i] = g_captured[i];
            g_capturedCount = 0;
        }
    }
    if (captured == 0) return ran;
    // Placed at the switch, or not at all: no attenuation or no location is silence, never 2D.
    void* att = ClickAttenuation();
    FVector at{};
    if (!att || !R::IsLive(sw) || !engine::TryGetActorLocation(sw, at) ||
        !std::isfinite(at.X) || !std::isfinite(at.Y) || !std::isfinite(at.Z))
        return ran;
    for (int i = 0; i < captured; ++i)
        if (R::IsLive(sounds[i].sound))
            engine::PlaySoundAtLocation(sw, sounds[i].sound, at, att, sounds[i].volume, sounds[i].pitch);
    return ran;
}

// --- The GROUP as a synced entity -----------------------------------------

void* SwitchRoot(void* sw) {
    if (!sw || g_objectsOff < 0 || !EnsureResolved()) return nullptr;
    const auto* arr = reinterpret_cast<const field_io::TArrayView*>(reinterpret_cast<const char*>(sw) + g_objectsOff);
    if (!arr->data || arr->num <= 0) return nullptr;
    void* first = *reinterpret_cast<void* const*>(arr->data);
    return (first && R::IsLive(first) && IsLightRoot(first)) ? first : nullptr;
}

bool CallRunTrigger(void* root, int32_t index) {
    if (!root || !g_runTriggerFn) return false;
    ParamFrame f(g_runTriggerFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"owner", root);   // never read by the BP on any index path; a valid object beats null
    f.Set<int32_t>(L"index", index);
    return Call(root, f);
}

bool ApplyGroupState(void* root, bool on) { return CallRunTrigger(root, on ? 1 : 2); }

bool TryReadBreaker(void* root, bool& open) {
    if (!root || g_breakerOff < 0) return false;
    open = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(root) + g_breakerOff);
    return true;
}

bool CallSetBreaker(void* root, bool open) {
    if (!root || !g_setActiveFn) return false;
    ParamFrame f(g_setActiveFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"active", open);
    return Call(root, f);
}

}  // namespace ue_wrap::lightswitch
