// coop/dev/lab_cmd.cpp -- see coop/dev/lab_cmd.h.

#include "coop/dev/lab_cmd.h"

#include "coop/config/config_registry.h"   // drill: a row by its key
#include "coop/dev/dev_lanes.h"   // drill: every drill back to its first phase
#include "coop/dev/director/aimed_grab.h"   // grab, apply
#include "coop/dev/rearm.h"   // drill: the knobs read again
#include "../config/config_internal.h"   // drill: the runtime layer alone, never the ini
#include "coop/dev/director/routes.h"   // route, ring
#include "coop/dev/drill_props.h"   // tp near: the floor under the stand
#include "coop/dev/lab_window.h"   // window show|hide, type (the parked window's own messages)

#include "coop/element/portable_identity.h"

#include "coop/net/session.h"
#include "coop/session/session_manager.h"
#include "coop/player/hand_item.h"      // the state file's hand lines
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/teleport_client.h"
#include "coop/props/prop_key_index.h"  // key:<Key> targets
#include "coop/props/remote_prop.h"     // ResolveMirrorEidByActor (grabintent)
#include "coop/props/trash_channel.h"   // SendGrabIntent / SendThrowIntent / ClientCarryEid
#include "coop/world/dimension_travel.h"
#include "harness/autotest.h"     // WorldCensusNowGT / ForceCollectionNowGT (worlds, gc)
#include "harness/screenshot.h"
#include "harness/session_runtime.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_fingerprint.h"
#include "ue_wrap/actors/sleep.h"   // dilation
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/gc_pin.h"       // ReportWorldScopedPins (pins)
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"     // use: the root a hit lands on
#include "ue_wrap/engine/engine_component.h"  // use: a component's place
#include "ue_wrap/engine/hit_result.h"        // use, apply: the hit the E press carries
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/world/daynightcycle.h"  // daytime

#include <windows.h>

#include <cctype>
#include <deque>
#include <memory>
#include <algorithm>
#include <cmath>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace coop::dev::lab_cmd {
namespace {

namespace R = ue_wrap::reflection;

bool g_init = false, g_on = false;
std::wstring g_dir, g_tag;
long long g_lastId = 0;
FILETIME g_lastWrite{};
std::chrono::steady_clock::time_point g_nextPoll{}, g_nextState{};
// Frames counted since the last state write, and the worst frame among them, for the fps line.
int g_frames = 0;
double g_worstMs = 0.0;
std::chrono::steady_clock::time_point g_lastFrame{}, g_lastStateAt{};

std::wstring Env(const wchar_t* name) {
    wchar_t buf[512] = {};
    const DWORD n = ::GetEnvironmentVariableW(name, buf, 512);
    return (n > 0 && n < 512) ? std::wstring(buf, n) : std::wstring();
}

std::wstring PathOf(const wchar_t* suffix) { return g_dir + L"\\" + g_tag + suffix; }

void Answer(long long id, bool ok, const std::string& text) {
    if (FILE* f = _wfopen(PathOf(L"_out.txt").c_str(), L"ab")) {
        std::fprintf(f, "%lld %s %s\r\n", id, ok ? "ok" : "fail", text.c_str());
        std::fclose(f);
    }
    UE_LOGI("[LAB] #%lld %s %s", id, ok ? "ok" : "FAIL", text.c_str());
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c > 0 && c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

bool CallNoParams(void* obj, const wchar_t* fnName) {
    if (!obj) return false;
    void* fn = R::FindFunction(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(obj, f);
}


std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}


// Live actors (not class defaults) whose class name holds `sub` (lowercase), with their distance to this player.
struct Near { void* obj; float dist; ue_wrap::FVector at; };
std::vector<Near> ActorsAround(const std::string& sub, float radius) {
    std::vector<Near> out;
    void* local = coop::players::Registry::Get().Local();
    ue_wrap::FVector me{};
    if (!local || !ue_wrap::engine::TryGetActorLocation(local, me)) return out;
    static void* sActor = nullptr;
    if (!sActor) sActor = R::FindClass(L"Actor");
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || obj == local || !R::IsLive(obj)) continue;
        const std::wstring cn = R::ClassNameOf(obj);
        if (cn.size() < 3 || cn.compare(cn.size() - 2, 2, L"_C") != 0) continue;
        if (!sub.empty() && Lower(Narrow(cn)).find(sub) == std::string::npos) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (!sActor || !R::IsDescendantOfAny(R::ClassOf(obj), &sActor, 1, 32)) continue;
        ue_wrap::FVector at{};
        if (!ue_wrap::engine::TryGetActorLocation(obj, at)) continue;
        const float dx = at.X - me.X, dy = at.Y - me.Y, dz = at.Z - me.Z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d <= radius) out.push_back({obj, d, at});
    }
    std::sort(out.begin(), out.end(), [](const Near& a, const Near& b) { return a.dist < b.dist; });
    return out;
}

// A target by name: player, gm, gi, slot (the save object), C:<Class> (the first live object of that class),
// near:<class text> (the nearest actor of such a class); <target>/<property> follows an object property of it.
void* Target(const std::string& t) {
    const size_t slash = t.rfind('/');  // a/b/c: c is a property of whatever a/b names
    if (slash != std::string::npos) {
        void* base = Target(t.substr(0, slash));
        if (!base) return nullptr;
        const int32_t off = R::FindPropertyOffset(R::ClassOf(base), Wide(t.substr(slash + 1)).c_str());
        if (off < 0) return nullptr;
        // The member may name an object already gone (a torch's flameBase after its own ignite): a dead one is no
        // target, never a class read through a stale pointer.
        void* member = *reinterpret_cast<void**>(static_cast<char*>(base) + off);
        return R::IsLive(member) ? member : nullptr;
    }
    if (t == "player") return coop::players::Registry::Get().Local();
    if (t == "gm") return ue_wrap::world_singleton::Gamemode();
    if (t == "gi") return ue_wrap::world_singleton::GameInstance();
    if (t == "slot") {
        void* gi = ue_wrap::world_singleton::GameInstance();
        if (!gi) return nullptr;
        const int32_t off = R::FindPropertyOffset(R::ClassOf(gi), L"save_gameInst");
        return off < 0 ? nullptr : *reinterpret_cast<void**>(static_cast<char*>(gi) + off);
    }
    if (t.rfind("C:", 0) == 0) return R::FindObjectByClass(Wide(t.substr(2)).c_str());
    if (t.rfind("puppet:", 0) == 0) {  // a peer's body here, by slot
        auto& p = coop::puppet_drive::Puppet(std::atoi(t.substr(7).c_str()));
        return p.valid() ? p.GetActor() : nullptr;
    }
    if (t.rfind("hand:", 0) == 0) {  // the item in a slot's hand: this peer's own hand actor, or its display mirror
        const int slot = std::atoi(t.substr(5).c_str());
        if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return nullptr;
        if (slot == coop::players::Registry::Get().LocalPeerId()) return coop::hand_item::LocalHandActor();
        return coop::hand_item::MirrorActorForSlot(static_cast<uint8_t>(slot));
    }
    if (t.rfind("key:", 0) == 0) return coop::prop_key_index::Find(Wide(t.substr(4)));  // a keyed prop by its key
    if (t.rfind("key:", 0) == 0) return coop::prop_key_index::Find(Wide(t.substr(4)));  // a keyed prop by its key
    if (t.rfind("near:", 0) == 0) {  // the nearest live actor whose class name holds the text
        const auto v = ActorsAround(Lower(t.substr(5)), 1e9f);
        return v.empty() ? nullptr : v.front().obj;
    }
    if (t.rfind("obj:", 0) == 0) {
        // obj:<Name>[@<Class>]: the first live object of exactly that name, and with @ only one whose
        // outer chain holds an object of that class -- a widget the designer left unexposed (no
        // variable to chain to), named inside the user widget that owns it: obj:Button_3@laptopUI_C.
        std::string name = t.substr(4), owner;
        const size_t at = name.find('@');
        if (at != std::string::npos) { owner = name.substr(at + 1); name = name.substr(0, at); }
        const std::wstring wname = Wide(name), wowner = Wide(owner);
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj || !R::IsLive(obj) || R::ToString(R::NameOf(obj)) != wname) continue;
            if (owner.empty()) return obj;
            for (void* o = R::OuterOf(obj); o; o = R::OuterOf(o))
                if (R::ClassNameOf(o) == wowner) return obj;
        }
        return nullptr;
    }
    return nullptr;
}

// A property of `owner` (a UClass* or UFunction*) at `base`, as text.
void* Target(const std::string& t);

std::string ReadValue(void* owner, void* base, const std::wstring& prop) {
    int32_t off = -1;
    std::wstring cls;
    if (!R::PropertyOffsetAndClass(owner, prop.c_str(), off, cls) || off < 0) return "?(no such property)";
    const char* p = static_cast<const char*>(base) + off;
    char buf[160];
    if (cls == L"FloatProperty") std::snprintf(buf, sizeof(buf), "%g", *reinterpret_cast<const float*>(p));
    else if (cls == L"IntProperty") std::snprintf(buf, sizeof(buf), "%d", *reinterpret_cast<const int32_t*>(p));
    else if (cls == L"ByteProperty") std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(*reinterpret_cast<const uint8_t*>(p)));
    else if (cls == L"BoolProperty") {
        int32_t bo = 0; uint8_t mask = 0;
        if (!R::FindBoolProperty(owner, prop.c_str(), bo, mask)) return "?(bool)";
        std::snprintf(buf, sizeof(buf), "%d", (static_cast<const uint8_t*>(base)[bo] & mask) ? 1 : 0);
    } else if (cls == L"ObjectProperty" || cls == L"ClassProperty") {
        void* o = *reinterpret_cast<void* const*>(p);
        if (!o) return "null";
        return Narrow(R::ClassNameOf(o)) + ":" + Narrow(R::ToString(R::NameOf(o)));
    } else if (cls == L"NameProperty") {
        return Narrow(R::ToString(*reinterpret_cast<const ue_wrap::reflection::FName*>(p)));
    } else if (cls == L"StrProperty") {
        return Narrow(ue_wrap::field_io::ReadFStringAt(base, off));
    } else if (cls == L"ArrayProperty") {
        std::snprintf(buf, sizeof(buf), "array n=%d", *reinterpret_cast<const int32_t*>(p + 8));
    } else if (cls == L"StructProperty" && prop.size()) {
        const float* f = reinterpret_cast<const float*>(p);
        std::snprintf(buf, sizeof(buf), "struct %g %g %g", f[0], f[1], f[2]);
    } else {
        std::snprintf(buf, sizeof(buf), "?(%s)", Narrow(cls).c_str());
    }
    return buf;
}

bool WriteValue(void* owner, void* base, const std::wstring& prop, const std::string& v, bool callFrame = false) {
    int32_t off = -1;
    std::wstring cls;
    if (!R::PropertyOffsetAndClass(owner, prop.c_str(), off, cls) || off < 0) return false;
    char* p = static_cast<char*>(base) + off;
    if (cls == L"FloatProperty") *reinterpret_cast<float*>(p) = std::strtof(v.c_str(), nullptr);
    else if (cls == L"IntProperty") *reinterpret_cast<int32_t*>(p) = std::atoi(v.c_str());
    else if (cls == L"ByteProperty") *reinterpret_cast<uint8_t*>(p) = static_cast<uint8_t>(std::atoi(v.c_str()));
    else if (cls == L"ObjectProperty") *reinterpret_cast<void**>(p) = (v == "null") ? nullptr : Target(v);
    else if (cls == L"NameProperty") *reinterpret_cast<R::FName*>(p) = ue_wrap::fname_utils::StringToFName(Wide(v).c_str());
    else if (cls == L"BoolProperty") {
        int32_t bo = 0; uint8_t mask = 0;
        if (!R::FindBoolProperty(owner, prop.c_str(), bo, mask)) return false;
        uint8_t& b = static_cast<uint8_t*>(base)[bo];
        b = (v == "1" || v == "true") ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
    } else if (cls == L"StrProperty" && callFrame) {
        // A call argument only: the frame's FString aliases a buffer this module keeps, and the callee
        // only reads it (the ATV's actionName compares its verb). Never into a live object, whose
        // destructor would free memory the engine did not allocate.
        static std::wstring keep[8];
        static int next = 0;
        std::wstring& w = keep[next++ % 8];
        w = Wide(v);
        *reinterpret_cast<R::FString*>(p) = R::FString{w.data(), static_cast<int32_t>(w.size()) + 1,
                                                      static_cast<int32_t>(w.size()) + 1};
    } else return false;
    return true;
}

void WriteText(const std::wstring& path, const std::string& text) {
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

// Every Blueprint function whose decoded text holds `needle` (lowercase), listed to <tag>_scan.txt.
std::string Scan(const std::string& needle) {
    std::ostringstream o;
    int fns = 0, hits = 0;
    ue_wrap::script_fingerprint::BeginPass();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || R::ClassNameOf(obj) != L"Function") continue;
        void* outer = R::OuterOf(obj);
        if (!outer) continue;
        const std::wstring on = R::ToString(R::NameOf(outer));
        if (on.size() < 2 || on.compare(on.size() - 2, 2, L"_C") != 0) continue;
        ++fns;
        const auto r = ue_wrap::script_fingerprint::Of(obj);
        if (r.native || r.text.find(needle) == std::string::npos) continue;
        ++hits;
        o << ue_wrap::script_fingerprint::PathOf(obj) << "\r\n";
        size_t pos = 0;
        int shown = 0;
        while ((pos = r.text.find(needle, pos)) != std::string::npos && shown < 4) {
            const size_t ls = r.text.rfind('\n', pos);
            const size_t le = r.text.find('\n', pos);
            const size_t a = ls == std::string::npos ? 0 : ls + 1;
            std::string line = r.text.substr(a, (le == std::string::npos ? r.text.size() : le) - a);
            if (line.size() > 300) line = line.substr(0, 300) + "...";
            o << "    " << line << "\r\n";
            pos = le == std::string::npos ? r.text.size() : le;
            ++shown;
        }
    }
    ue_wrap::script_fingerprint::EndPass();
    WriteText(PathOf(L"_scan.txt"), o.str());
    return "scan '" + needle + "': " + std::to_string(hits) + " of " + std::to_string(fns) + " BP functions";
}

// One function's decoded text (by Class and Function) to <tag>_bp_<Function>.txt.
std::string Dump(const std::string& cls, const std::string& fn) {
    void* c = R::FindClass(Wide(cls).c_str());
    void* f = c ? R::FindFunction(c, Wide(fn).c_str()) : nullptr;
    if (!f) return "";
    const auto r = ue_wrap::script_fingerprint::Of(f);
    std::string safe = fn;
    for (char& ch : safe) if (ch == ' ' || ch == '\\' || ch == '/' || ch == ':') ch = '_';
    WriteText(PathOf((L"_bp_" + Wide(cls) + L"_" + Wide(safe) + L".txt").c_str()), r.text);
    return cls + "." + fn + ": " + std::to_string(r.text.size()) + " bytes";
}

// The own and inherited fields of a target, filtered, as name=value lines in <tag>_fields.txt.
std::string Fields(void* obj, const std::string& filter) {
    std::ostringstream o;
    int n = 0;
    for (void* c = R::ClassOf(obj); c; c = R::SuperStructOf(c)) {
        for (const auto& fi : R::EnumerateStructFields(c)) {
            const std::string name = Narrow(fi.name);
            if (!filter.empty() && Lower(name).find(filter) == std::string::npos) continue;
            o << Narrow(R::ToString(R::NameOf(c))) << "." << name << " = " << ReadValue(c, obj, fi.name) << "\r\n";
            ++n;
        }
    }
    WriteText(PathOf(L"_fields.txt"), o.str());
    return std::to_string(n) + " fields";
}

// Call a function on a target with name=value arguments (floats, ints, bools, bytes).
bool CallWithArgs(void* obj, const std::string& fnName, std::istringstream& in, std::string& out) {
    void* decl = nullptr;
    void* fn = R::FindDispatchFunction(R::ClassOf(obj), Wide(fnName).c_str(), &decl);
    if (!fn) { out = " no function"; return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) { out = "bad frame"; return false; }
    std::string kv;
    while (in >> kv) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string val = kv.substr(eq + 1);
        if (!WriteValue(fn, f.data(), Wide(kv.substr(0, eq)), val, /*callFrame=*/true)) { out = "bad arg " + kv; return false; }
    }
    const bool ok = ue_wrap::Call(obj, f);
    std::ostringstream o;
    for (const auto& p : R::FunctionParams(fn))
        if (p.flags & 0x100 /*CPF_OutParm*/ || p.flags & 0x400 /*CPF_ReturnParm*/)
            o << " " << Narrow(p.name) << "=" << ReadValue(fn, f.data(), p.name);
    out = o.str();
    return ok;
}


// ---- timed input: the game's own input events, called as a key press would ----------------------
// A blueprint's InputAction node compiles to one event per edge (InpActEvt_<action>_K2Node_
// InputActionEvent_<n>, the lower n is the press, the next the release); an InputAxis node to one
// event the engine calls every frame with the axis value. Calling them is what the player's keys do,
// minus the key itself, so a lab copy in a background window can walk, drive, use and look.
using Clock = std::chrono::steady_clock;

struct Timed {
    void* obj = nullptr;
    int32_t idx = -1;        // the object's slot at the start, so a recycled address is never called
    void* fn = nullptr;      // axis: called every tick; action: the release, called once at `until`
    bool axis = false;
    float value = 0.f;
    Clock::time_point until{};
    std::string what;
};
std::vector<Timed> g_timed;

// The input events of `obj` for `name`, lowest node number first.
std::vector<void*> InputEvents(void* obj, bool axis, const std::string& name) {
    std::vector<void*> out;
    if (!obj) return out;
    void* cls = R::ClassOf(obj);
    const std::wstring base = std::wstring(axis ? L"InpAxisEvt_" : L"InpActEvt_") + Wide(name) +
                              (axis ? L"_K2Node_InputAxisEvent_" : L"_K2Node_InputActionEvent_");
    for (int i = 0; i < 160; ++i) {
        void* fn = R::FindDispatchFunction(cls, (base + std::to_wstring(i)).c_str(), nullptr);
        if (fn) out.push_back(fn);
    }
    return out;
}

// The press and release events of an action, read from the blueprint's own input bindings: each
// class in the chain keeps its InputActionDelegateBinding objects in DynamicBindingObjects, and each
// row names the action, the edge (IE_Pressed 0, IE_Released 1) and the event function it calls. A
// node number says nothing about the edge (the player's use has three events from two nodes), so the
// rows decide; only when none are found does the lowest-number-first guess stand.
struct ActionPair { std::vector<void*> press, release; int rows = 0; };  // every bound row fires, as the engine fires them

ActionPair ActionEvents(void* obj, const std::string& action) {
    ActionPair out;
    if (!obj) return out;
    const std::string want = Lower(action);
    for (void* cls = R::ClassOf(obj); cls; cls = R::SuperStructOf(cls)) {
        void* meta = R::ClassOf(cls);
        const int32_t dynOff = meta ? R::FindPropertyOffset(meta, L"DynamicBindingObjects") : -1;
        if (dynOff < 0) break;  // a native class: no blueprint bindings above it
        struct Arr { void** data; int32_t num; int32_t max; };
        const Arr& objs = *reinterpret_cast<const Arr*>(static_cast<uint8_t*>(cls) + dynOff);
        for (int32_t i = 0; i < objs.num && i < 64; ++i) {
            void* b = objs.data ? objs.data[i] : nullptr;
            if (!b || R::ClassNameOf(b) != L"InputActionDelegateBinding") continue;
            const int32_t rowsOff = R::FindPropertyOffset(R::ClassOf(b), L"InputActionDelegateBindings");
            if (rowsOff < 0) continue;
            struct RowArr { uint8_t* data; int32_t num; int32_t max; };
            const RowArr& rows = *reinterpret_cast<const RowArr*>(static_cast<uint8_t*>(b) + rowsOff);
            // FBlueprintInputActionDelegateBinding (4.27): the flag byte, InputActionName @4,
            // InputKeyEvent @12, FunctionNameToBind @16; 24 bytes a row.
            for (int32_t r = 0; r < rows.num && r < 512; ++r) {
                const uint8_t* row = rows.data + 24 * r;
                const R::FName an = *reinterpret_cast<const R::FName*>(row + 4);
                if (Lower(Narrow(R::ToString(an))) != want) continue;
                const uint8_t edge = row[12];
                const R::FName fnName = *reinterpret_cast<const R::FName*>(row + 16);
                void* fn = R::FindDispatchFunction(R::ClassOf(obj), R::ToString(fnName).c_str(), nullptr);
                ++out.rows;
                if (!fn) continue;
                auto& v = edge == 0 ? out.press : edge == 1 ? out.release : out.press;
                if (edge > 1) continue;  // repeat / double-click rows are not a key's press or release
                if (std::find(v.begin(), v.end(), fn) == v.end()) v.push_back(fn);
            }
        }
    }
    return out;
}

// One input event, as the game's own call (the sync hooks treat it as the player's act).
bool CallInput(void* obj, void* fn, bool axis, float value) {
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (axis) f.SetRaw(L"AxisValue", &value, sizeof(value));  // an action event's Key stays None
    R::SetGameAttributedDepth(1);
    const bool ok = ue_wrap::Call(obj, f);
    R::SetGameAttributedDepth(0);
    return ok;
}

void TickTimed(Clock::time_point now) {
    for (size_t i = 0; i < g_timed.size();) {
        Timed& t = g_timed[i];
        const bool live = R::IsLiveByIndex(t.obj, t.idx);
        if (t.axis && live && now < t.until) { CallInput(t.obj, t.fn, true, t.value); ++i; continue; }
        if (now < t.until && live) { ++i; continue; }
        if (live) {
            if (t.axis) CallInput(t.obj, t.fn, true, 0.f);   // the stick back to rest
            else if (t.fn) CallInput(t.obj, t.fn, false, 0.f);  // the key let go
        }
        UE_LOGI("[LAB] input %s ended%s", t.what.c_str(), live ? "" : " (its object is gone)");
        g_timed.erase(g_timed.begin() + static_cast<long>(i));
    }
}

void ReleaseAll() {
    for (Timed& t : g_timed) t.until = Clock::now();
    TickTimed(Clock::now());
}

// ---- keep fed: a lab body that never starves or tires --------------------------------------------
bool g_keepFed = false;
Clock::time_point g_nextFeed{};

namespace V = ue_wrap::vitals;

bool FieldByName(const std::string& n, V::Field& out) {
    for (uint8_t i = 0; i < static_cast<uint8_t>(V::Field::Count); ++i) {
        const V::Field f = static_cast<V::Field>(i);
        if (Lower(V::RowOf(f).name) == Lower(n)) { out = f; return true; }
    }
    return false;
}

void Feed() {
    float maxHealth = 100.f;
    V::Read(V::Field::MaxHealth, &maxHealth);
    V::Write(V::Field::Food, 100.f);
    V::Write(V::Field::Sleep, 100.f);
    V::Write(V::Field::Health, maxHealth > 0.f ? maxHealth : 100.f);
}


// ---- the lab camera: a CameraActor the player's view is handed to, for screenshots from outside ----
void* g_cam = nullptr;
int32_t g_camIdx = -1;

void* LocalController() {
    void* local = coop::players::Registry::Get().Local();
    return local ? ue_wrap::engine::GetController(local) : nullptr;
}

bool SetViewTarget(void* ctrl, void* target) {
    if (!ctrl || !target) return false;
    void* fn = R::FindDispatchFunction(R::ClassOf(ctrl), L"SetViewTargetWithBlend", nullptr);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.SetRaw(L"NewViewTarget", &target, sizeof(target))) return false;
    return ue_wrap::Call(ctrl, f);  // BlendTime 0: a cut
}

// The listing of a class's functions (its own and its parents' up to Actor), with their parameters.
std::string Funcs(const std::string& t, const std::string& filter) {
    void* cls = nullptr;
    if (void* obj = Target(t)) cls = R::ClassOf(obj);
    if (!cls) cls = R::FindClass(Wide(t).c_str());
    if (!cls) return "";
    std::ostringstream o;
    int n = 0;
    const std::string want = Lower(filter);
    for (int depth = 0; cls && depth < 8; ++depth, cls = R::SuperStructOf(cls)) {
        const std::string cn = Narrow(R::ToString(R::NameOf(cls)));
        if (cn == "Actor" || cn == "Object" || cn == "Pawn" || cn == "Character") break;
        o << "== " << cn << "\r\n";
        void* f = *reinterpret_cast<void**>(static_cast<uint8_t*>(cls) + 0x48);  // UStruct::Children
        for (int hops = 0; f && hops < 65536; ++hops) {
            if (R::ClassNameOf(f) == L"Function") {
                const std::string fn = Narrow(R::ToString(R::NameOf(f)));
                if (want.empty() || Lower(fn).find(want) != std::string::npos) {
                    o << fn << "(";
                    bool first = true;
                    for (const auto& prm : R::FunctionParams(f)) {
                        o << (first ? "" : ", ") << Narrow(prm.name) << ((prm.flags & 0x400) ? ":ret" : (prm.flags & 0x100) ? ":out" : "");
                        first = false;
                    }
                    o << ")\r\n";
                    ++n;
                }
            }
            f = *reinterpret_cast<void**>(static_cast<uint8_t*>(f) + 0x28);  // UField::Next
        }
    }
    WriteText(PathOf(L"_funcs.txt"), o.str());
    return std::to_string(n) + " function(s) in " + Narrow(g_tag) + "_funcs.txt";
}


// ---- screens: the widgets a player clicks, reached without a mouse ---------------------------------
// A UMG button's click is its OnClicked multicast delegate broadcast: every function the screen's
// blueprint bound to it (BndEvt__<Button>_..._OnButtonClickedEvent) runs, which is all a mouse click
// on it does past Slate's hit test. The lab copy's window is off-screen and its cursor is the owner's,
// so the hit test is the one part a lab click skips; the screen's own logic runs whole.

// FScriptDelegate (4.27): the bound object's weak pointer {ObjectIndex, SerialNumber} and the
// function's FName; a multicast delegate's invocation list is a TArray of them.
struct ScriptDelegate { int32_t objIdx; int32_t serial; R::FName fn; };
static_assert(sizeof(ScriptDelegate) == 16, "FScriptDelegate is 16 bytes in 4.27");

int Broadcast(void* obj, const std::wstring& delegateName, std::string& out) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), delegateName.c_str());
    if (off < 0) { out = " no delegate " + Narrow(delegateName); return -1; }
    struct Arr { ScriptDelegate* data; int32_t num; int32_t max; };
    const Arr& arr = *reinterpret_cast<const Arr*>(static_cast<uint8_t*>(obj) + off);
    // A copy first: a handler may rebind the button (a screen that swaps its page does).
    std::vector<ScriptDelegate> list(arr.data, arr.data + (std::max)(0, (std::min)(arr.num, 64)));
    int ran = 0;
    for (const ScriptDelegate& d : list) {
        void* target = R::ResolveWeakObject(d.objIdx, d.serial);
        const std::wstring fnName = R::ToString(d.fn);
        void* fn = target ? R::FindDispatchFunction(R::ClassOf(target), fnName.c_str(), nullptr) : nullptr;
        out += " " + Narrow(fnName) + (fn ? "" : "(gone)");
        if (!fn) continue;
        ue_wrap::ParamFrame f(fn);  // a click's signature has no parameters; any other stays zeroed
        if (!f.valid()) continue;
        R::SetGameAttributedDepth(1);  // the player's own act: the sync lanes see it as a change to send
        if (ue_wrap::Call(target, f)) ++ran;
        R::SetGameAttributedDepth(0);
    }
    return ran;
}

// widgets: with no target, every live user widget (a screen, a menu, a HUD part) by class; with one,
// every object inside it (its widget tree: buttons, text, edit boxes), each with its class, and a
// button's bound click count. Into <tag>_widgets.txt.
std::string Widgets(void* root, const std::string& filter) {
    static void* sUserWidget = nullptr;
    if (!sUserWidget) sUserWidget = R::FindClass(L"UserWidget");
    std::ostringstream o;
    int n = 0;
    const int32_t count = R::NumObjects();
    for (int32_t i = 0; i < count; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || obj == root || !R::IsLive(obj)) continue;
        const std::wstring name = R::ToString(R::NameOf(obj));
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const std::wstring cn = R::ClassNameOf(obj);
        if (root) {
            bool inside = false;
            int depth = 0;
            for (void* p = R::OuterOf(obj); p && depth < 8 && !inside; p = R::OuterOf(p), ++depth) inside = (p == root);
            if (!inside) continue;
        } else if (!sUserWidget || !R::IsDescendantOfAny(R::ClassOf(obj), &sUserWidget, 1, 32)) {
            continue;
        } else if (filter.empty()) {
            // The screens themselves: a user widget nested in another's tree (a slider of a server
            // minigame, a row of a list) is listed under its owner, with a target or a filter.
            void* outer = R::OuterOf(obj);
            if (outer && R::ClassNameOf(outer) == L"WidgetTree") continue;
        }
        const std::string line = Narrow(cn) + " " + Narrow(name);
        if (!filter.empty() && Lower(line).find(filter) == std::string::npos) continue;
        o << line;
        const int32_t clickOff = R::FindPropertyOffset(R::ClassOf(obj), L"OnClicked");
        if (clickOff >= 0) o << "  OnClicked n=" << *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(obj) + clickOff + 8);
        if (void* outer = R::OuterOf(obj)) o << "  in " << Narrow(R::ClassNameOf(outer)) << ":" << Narrow(R::ToString(R::NameOf(outer)));
        o << "\r\n";
        if (++n >= 2000) break;
    }
    WriteText(PathOf(L"_widgets.txt"), o.str());
    return std::to_string(n) + " widget(s) in " + Narrow(g_tag) + "_widgets.txt";
}


// ---- watches: named values written into the state file every second, for comparing peers ---------
// The same name watched on the host and on the client gives the lab's `compare` a pair to hold side by
// side: a door's open flag, a disc's file count, a prop's location. The target is resolved once and
// kept while it lives (a near: search every second would cost frames the fps line then blames on us).
struct Watch {
    std::string name, target, prop;
    void* obj = nullptr;
    int32_t idx = -1;
};
std::vector<Watch> g_watches;

std::string WatchValue(Watch& w) {
    if (!w.obj || !R::IsLiveByIndex(w.obj, w.idx)) {
        w.obj = Target(w.target);
        w.idx = w.obj ? R::InternalIndexOf(w.obj) : -1;
    }
    if (!w.obj) return "-";
    char b[96];
    if (w.prop == "@loc" || w.prop == "@rot") {
        if (w.prop == "@loc") {
            ue_wrap::FVector l{};
            if (!ue_wrap::engine::TryGetActorLocation(w.obj, l) && !ue_wrap::engine::TryGetComponentLocation(w.obj, l)) return "?";  // an actor, or a component
            std::snprintf(b, sizeof(b), "%.0f %.0f %.0f", l.X, l.Y, l.Z);
        } else {
            ue_wrap::FRotator r{};
            if (!ue_wrap::engine::TryGetActorRotation(w.obj, r)) return "?";
            std::snprintf(b, sizeof(b), "%.0f %.0f %.0f", r.Pitch, r.Yaw, r.Roll);
        }
        return b;
    }
    if (w.prop == "@live") return "1";
    if (w.prop == "@class") return Narrow(R::ClassNameOf(w.obj));
    return ReadValue(R::ClassOf(w.obj), w.obj, Wide(w.prop));
}


// ---- smoothness: how a peer's puppet moves here, frame by frame -------------------------------------
// The eye's complaint about a remote body is a step: a frame it jumps (a late packet caught up), a run
// of frames it stands while it should walk (the buffer ran dry), a speed that changes every frame (the
// interpolation hunting). Each frame's displacement is kept for the second and reduced to those three
// at the state write, so a run can say "smooth" with numbers instead of a screenshot.
struct Track {
    bool have = false;
    ue_wrap::FVector last{};
    std::chrono::steady_clock::time_point lastAt{};
    std::vector<float> d, dt;  // this second's per-frame displacement (cm) and frame time (s)
};
Track g_track[coop::players::kMaxPeers];

void SamplePuppets(std::chrono::steady_clock::time_point now) {
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        Track& t = g_track[slot];
        auto& p = coop::puppet_drive::Puppet(slot);
        ue_wrap::FVector l{};
        if (!p.valid() || !p.TryGetLocation(l)) { t = Track{}; continue; }
        if (t.have && t.d.size() < 1000) {
            const float dx = l.X - t.last.X, dy = l.Y - t.last.Y, dz = l.Z - t.last.Z;
            t.d.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
            t.dt.push_back(static_cast<float>(std::chrono::duration<double>(now - t.lastAt).count()));
        }
        t.have = true;
        t.last = l;
        t.lastAt = now;
    }
}

// One line per puppet: frames, mean speed, the mean and worst frame-to-frame change of speed, the
// frames that jumped (over four times the median step and 10 cm), and the frames it stood still while
// the second's mean speed says it walks.
std::string SmoothLines() {
    std::ostringstream o;
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        Track& t = g_track[slot];
        if (t.d.size() < 2) { t.d.clear(); t.dt.clear(); continue; }
        double sumD = 0, sumT = 0;
        for (size_t i = 0; i < t.d.size(); ++i) { sumD += t.d[i]; sumT += t.dt[i]; }
        const double speed = sumT > 0 ? sumD / sumT : 0;
        std::vector<float> sorted = t.d;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(sorted.size() / 2), sorted.end());
        const float median = sorted[sorted.size() / 2];
        int pops = 0, still = 0;
        double dvSum = 0, dvMax = 0, prevV = -1;
        for (size_t i = 0; i < t.d.size(); ++i) {
            if (t.d[i] > 4.f * median && t.d[i] > 10.f) ++pops;
            if (speed > 30.0 && t.d[i] < 0.01f) ++still;
            const double v = t.dt[i] > 0 ? t.d[i] / t.dt[i] : 0;
            if (prevV >= 0) { const double dv = std::fabs(v - prevV); dvSum += dv; dvMax = (std::max)(dvMax, dv); }
            prevV = v;
        }
        char b[192];
        std::snprintf(b, sizeof(b), "smooth %d n=%zu speed=%.0f dv=%.0f dvmax=%.0f pops=%d still=%d\r\n", slot,
                      t.d.size(), speed, dvSum / static_cast<double>(t.d.size() - 1), dvMax, pops, still);
        o << b;
        t.d.clear();
        t.dt.clear();
    }
    return o.str();
}

// ---- a player's own verbs on a thing: use, grab, apply -----------------------------------------------
// The component of `actor` named `name` (an object of that name whose outer chain holds the actor), or null.
void* ComponentNamed(void* actor, const std::string& name) {
    const std::wstring w = Wide(name);
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !R::IsLive(obj) || R::ToString(R::NameOf(obj)) != w) continue;
        for (void* o = R::OuterOf(obj); o; o = R::OuterOf(o))
            if (o == actor) return obj;
    }
    return nullptr;
}

// The verb `fnName` of `obj` with whichever of the E press's parameters it declares: this player, the hit on `comp`
// (the actor's root when null) at its place, the looked-at component, the action byte, and the hand's held actor.
// Attributed to the game, so the lanes read it as a player's press. False when the function does not resolve or
// the call did not run; `out` names what was filled.
bool PlayerVerb(void* obj, void* comp, const wchar_t* fnName, void* player, int action, void* held, std::string& out) {
    void* fn = R::FindDispatchFunction(R::ClassOf(obj), fnName, nullptr);
    if (!fn) { out = " no " + Narrow(fnName); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) { out = " bad frame"; return false; }
    void* at = comp ? comp : ue_wrap::engine::RootComponentOf(obj);
    ue_wrap::FVector loc{};
    if (comp) loc = ue_wrap::engine::GetComponentLocation(comp);
    else ue_wrap::engine::TryGetActorLocation(obj, loc);
    out.clear();
    if (f.Set<void*>(L"player", player) || f.Set<void*>(L"Player", player)) out += " player";
    if (at && (ue_wrap::hit_result::Write(f, L"hit", obj, at, loc) || ue_wrap::hit_result::Write(f, L"Hit", obj, at, loc)))
        out += " hit";
    if (at && f.Set<void*>(L"lookAtComponent", at)) out += " lookAt";
    if (action >= 0 && (f.Set<uint8_t>(L"action", static_cast<uint8_t>(action)) || f.Set<uint8_t>(L"Action", static_cast<uint8_t>(action)))) out += " action=" + std::to_string(action);
    if (held && f.Set<void*>(L"holdObject", held)) out += " held";
    R::SetGameAttributedDepth(1);
    const bool ok = ue_wrap::Call(obj, f);
    R::SetGameAttributedDepth(0);
    return ok;
}

// The one grab in flight: the director's aimed grab, ticked every frame, answered when it ends.
struct PendingGrab {
    long long id = 0;
    std::unique_ptr<coop::director::AimedGrab> grab;
    std::chrono::steady_clock::time_point until{};
};
PendingGrab g_grab;

void TickGrab(std::chrono::steady_clock::time_point now) {
    if (!g_grab.grab) return;
    const coop::director::GrabState st = g_grab.grab->Tick();
    if (st == coop::director::GrabState::Working && now < g_grab.until) return;
    const bool ok = st == coop::director::GrabState::Grabbed;
    Answer(g_grab.id, ok, ok ? "grab: held after " + std::to_string(g_grab.grab->AimPoses()) + " aim pose(s)"
                             : std::string("grab: ") + (st == coop::director::GrabState::Failed ? g_grab.grab->Why()
                                                                                                    : "timed out"));
    g_grab.grab.reset();
}

// A stand `dist` cm from `obj` (an actor or a component), facing it: tried round it every 30 degrees from the side this
// player is on, the first whose floor (found within 3 m under the target's height) lies no more than 2.5 m under the
// target -- a lift button's near side is its shaft, and a stand over a pit drops the player down it. False when the
// target or the player does not read, or no side has such a floor.
bool StandNear(void* local, void* obj, float dist, ue_wrap::FVector& stand, float& yaw) {
    ue_wrap::FVector at{}, me{};
    if (!ue_wrap::engine::TryGetActorLocation(obj, at) && !ue_wrap::engine::TryGetComponentLocation(obj, at)) return false;
    if (!ue_wrap::engine::TryGetActorLocation(local, me)) return false;
    const float base = std::atan2(me.Y - at.Y, me.X - at.X);
    for (int i = 0; i < 12; ++i) {
        const int k = (i + 1) / 2 * ((i % 2) ? 1 : -1);   // 0, +1, -1, +2, -2, ... steps of 30 degrees
        const float a = base + static_cast<float>(k) * 3.14159265f / 6.f;
        const float x = at.X + std::cos(a) * dist, y = at.Y + std::sin(a) * dist;
        float floorZ = 0.f;
        if (!coop::dev::drill_props::FloorAt(local, x, y, at.Z + 150.f, 450.f, 20.f, &floorZ) || floorZ < at.Z - 250.f)
            continue;
        stand = {x, y, floorZ + 95.f};
        yaw = static_cast<float>(std::atan2(at.Y - y, at.X - x) * 180.0 / 3.14159265358979);
        return true;
    }
    return false;
}

void Run(long long id, const std::string& verb, const std::string& rest) {
    std::istringstream in(rest);
    void* local = coop::players::Registry::Get().Local();
    if (verb == "tp") {
        // tp <x> <y> <z> [yaw] | tp near <target> [dist]: there, or `dist` cm (120) from the target on this player's
        // side, on the floor found there, facing it.
        float x = 0, y = 0, z = 0, yaw = 0;
        std::string first;
        in >> first;
        if (first == "near") {
            std::string t;
            float dist = 120.f;
            in >> t;
            if (!(in >> dist)) dist = 120.f;
            void* obj = Target(t);
            ue_wrap::FVector st{};
            if (!obj || !local || !StandNear(local, obj, dist, st, yaw)) { Answer(id, false, "tp: no target " + t + ", or no floor beside it"); return; }
            x = st.X; y = st.Y; z = st.Z;
        } else {
            x = static_cast<float>(std::atof(first.c_str()));
            in >> y >> z >> yaw;
        }
        const bool ok = coop::teleport_client::ApplyLocally({x, y, z, 0.f, yaw, 0.f});
        Answer(id, ok, "tp " + rest);
    } else if (verb == "travel") {
        std::string level;
        in >> level;
        const bool ok = coop::dimension_travel::TravelAsGameEntry(std::wstring(level.begin(), level.end()));
        Answer(id, ok, "travel " + level);
    } else if (verb == "exec") {
        const std::wstring cmd(rest.begin(), rest.end());
        Answer(id, ue_wrap::engine::ExecuteConsoleCommand(cmd.c_str()), "exec " + rest);
    } else if (verb == "shot") {
        const std::wstring label(rest.begin(), rest.end());
        Answer(id, harness::screenshot::Capture(label.c_str()), "shot " + rest);
    } else if (verb == "callp" || verb == "callgm") {
        std::string fn;
        in >> fn;
        void* obj = verb == "callp" ? local : ue_wrap::world_singleton::Gamemode();
        Answer(id, CallNoParams(obj, std::wstring(fn.begin(), fn.end()).c_str()), verb + " " + fn);
    } else if (verb == "scan") {
        std::string needle;
        std::getline(in, needle);
        const size_t b = needle.find_first_not_of(' ');
        needle = b == std::string::npos ? std::string() : Lower(needle.substr(b));
        Answer(id, !needle.empty(), needle.empty() ? "scan: empty" : Scan(needle));
    } else if (verb == "dump") {
        std::string cls, fn;
        in >> cls;
        std::getline(in, fn);
        const size_t b = fn.find_first_not_of(' ');
        fn = b == std::string::npos ? std::string() : fn.substr(b);
        const std::string r = Dump(cls, fn);
        Answer(id, !r.empty(), r.empty() ? "dump: not found " + cls + "." + fn : r);
    } else if (verb == "get" || verb == "set" || verb == "fields") {
        std::string t, prop, val;
        in >> t >> prop >> val;
        void* obj = Target(t);
        if (!obj) { Answer(id, false, verb + ": no target " + t); return; }
        if (verb == "fields") { Answer(id, true, Fields(obj, Lower(prop))); return; }
        if (verb == "get" && !prop.empty() && prop[0] == '@') {   // @loc @rot @class @live, as a watch reads them
            Watch w;
            w.target = t;
            w.prop = prop;
            Answer(id, true, t + "." + prop + " = " + WatchValue(w));
            return;
        }
        if (verb == "get" && !prop.empty() && prop[0] == '@') {   // @loc @rot @class @live, as a watch reads them
            Watch w;
            w.target = t;
            w.prop = prop;
            Answer(id, true, t + "." + prop + " = " + WatchValue(w));
            return;
        }
        if (verb == "set" && !WriteValue(R::ClassOf(obj), obj, Wide(prop), val)) { Answer(id, false, "set failed " + prop); return; }
        Answer(id, true, t + "." + prop + " = " + ReadValue(R::ClassOf(obj), obj, Wide(prop)));
    } else if (verb == "move") {
        // move <target> <x> <y> <z>: the actor teleported there (a disc dropped into a slot's mouth, say).
        std::string t;
        float x = 0, y = 0, z = 0;
        in >> t >> x >> y >> z;
        void* obj = Target(t);
        if (!obj) { Answer(id, false, "move: no target " + t); return; }
        // The engine's teleport refuses some actors (a simulating prop far from where it was): a snap then.
        const bool ok = ue_wrap::engine::TeleportTo(obj, ue_wrap::FVector{x, y, z}, ue_wrap::FRotator{0.f, 0.f, 0.f}) ||
                        ue_wrap::engine::SetActorLocation(obj, ue_wrap::FVector{x, y, z});
        Answer(id, ok, "move " + t);
    } else if (verb == "rows" || verb == "addrow") {
        // rows <target> <field>: a TArray<FString> read out; addrow <target> <field> <text>: one row appended
        // ('+' for a space), as a device writes a file onto a disc.
        std::string t, prop, text;
        in >> t >> prop >> text;
        for (char& c : text) if (c == '+') c = ' ';
        void* obj = Target(t);
        if (!obj) { Answer(id, false, verb + ": no target " + t); return; }
        const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), Wide(prop).c_str());
        if (off < 0) { Answer(id, false, verb + ": no field " + prop); return; }
        std::vector<std::wstring> v = ue_wrap::field_io::ReadFStringArrayField(obj, off);
        if (verb == "addrow") {
            v.push_back(Wide(text));
            if (!ue_wrap::field_io::WriteFStringArrayField(obj, off, v)) { Answer(id, false, "addrow: write failed"); return; }
        }
        std::string all;
        for (const auto& w : v) all += (all.empty() ? "" : " | ") + Narrow(w);
        Answer(id, true, t + "." + prop + " n=" + std::to_string(v.size()) + " [" + all.substr(0, 300) + "]");
    } else if (verb == "names" || verb == "addname" || verb == "clearnames") {
        // names <target> <field>: a TArray<FName> read out; addname <target> <field> <name>: one appended;
        // clearnames <target> <field>: emptied (an Omega's worn accessory list, say).
        std::string t, prop, text;
        in >> t >> prop >> text;
        void* obj = Target(t);
        if (!obj) { Answer(id, false, verb + ": no target " + t); return; }
        const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), Wide(prop).c_str());
        if (off < 0) { Answer(id, false, verb + ": no field " + prop); return; }
        struct NameArr { R::FName* data; int32_t num; int32_t max; };
        auto* arr = reinterpret_cast<NameArr*>(static_cast<char*>(obj) + off);
        if (verb == "clearnames") arr->num = 0;
        if (verb == "addname") {
            if (arr->num >= arr->max) {
                const int32_t cap = arr->max + 8;
                auto* buf = static_cast<R::FName*>(R::EngineAlloc(sizeof(R::FName) * static_cast<size_t>(cap)));
                if (!buf) { Answer(id, false, "addname: alloc failed"); return; }
                for (int32_t i = 0; i < arr->num; ++i) buf[i] = arr->data[i];
                if (arr->data) R::EngineFree(arr->data);
                arr->data = buf;
                arr->max = cap;
            }
            arr->data[arr->num++] = ue_wrap::fname_utils::StringToFName(Wide(text));
        }
        std::string all;
        for (int32_t i = 0; i < arr->num && i < 32; ++i) all += (i ? " | " : "") + Narrow(R::ToString(arr->data[i]));
        Answer(id, true, t + "." + prop + " n=" + std::to_string(arr->num) + " [" + all + "]");
    } else if (verb == "classes") {
        // classes <text>: every loaded class whose name holds the text (any case), spelled as the engine has it.
        std::string sub;
        in >> sub;
        const std::string want = Lower(sub);
        std::string all;
        int n = 0;
        const int32_t count = R::NumObjects();
        for (int32_t i = 0; i < count && n < 40; ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj) continue;
            const std::wstring cn = R::ClassNameOf(obj);
            if (cn != L"BlueprintGeneratedClass" && cn != L"Class") continue;
            const std::string nm = Narrow(R::ToString(R::NameOf(obj)));
            if (Lower(nm).find(want) == std::string::npos) continue;
            all += (n++ ? " | " : "") + nm;
        }
        Answer(id, true, "classes '" + sub + "': " + all);
    } else if (verb == "call" || verb == "gcall") {
        std::string t, fn;
        in >> t >> fn;
        for (char& c : fn) if (c == '+') c = ' ';  // a function name with spaces is written with '+'
        void* obj = Target(t);
        if (!obj) { Answer(id, false, "call: no target " + t); return; }
        std::string out;
        // gcall: the call stands for the game's own (a player's verb), so the lanes that ignore our own
        // applies see it as a change to send.
        if (verb == "gcall") R::SetGameAttributedDepth(1);
        const bool ok = CallWithArgs(obj, fn, in, out);
        if (verb == "gcall") R::SetGameAttributedDepth(0);
        Answer(id, ok, verb + " " + t + "." + fn + out);
    } else if (verb == "look" || verb == "face") {
        // look <yaw> [pitch]: turn this player's view; face <slot>: turn it toward that peer's puppet.
        float yaw = 0.f, pitch = 0.f;
        if (verb == "look") {
            // look <yaw> [pitch], or look at <target>: the camera turned onto that actor's origin.
            std::string first;
            in >> first;
            if (first == "at") {
                std::string t;
                in >> t;
                void* obj = Target(t);
                ue_wrap::FVector there{};
                if (!obj || !ue_wrap::engine::TryGetActorLocation(obj, there)) {
                    Answer(id, false, "look: no target " + t);
                    return;
                }
                const ue_wrap::FVector cam = ue_wrap::engine::GetCameraLocation();
                const double dx = there.X - cam.X, dy = there.Y - cam.Y, dz = there.Z - cam.Z;
                yaw = static_cast<float>(std::atan2(dy, dx) * 180.0 / 3.14159265358979);
                pitch = static_cast<float>(std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.0 / 3.14159265358979);
            } else {
                yaw = static_cast<float>(std::atof(first.c_str()));
                in >> pitch;
            }
        } else {
            int slot = -1;
            in >> slot;
            ue_wrap::FVector me{}, them{};
            auto& p = coop::puppet_drive::Puppet(slot);
            if (!local || !p.valid() || !ue_wrap::engine::TryGetActorLocation(local, me) || !p.TryGetLocation(them)) {
                Answer(id, false, "face: no puppet in slot " + std::to_string(slot));
                return;
            }
            yaw = static_cast<float>(std::atan2(them.Y - me.Y, them.X - me.X) * 180.0 / 3.14159265358979);
        }
        void* ctrl = local ? ue_wrap::engine::GetController(local) : nullptr;
        if (ctrl) ue_wrap::engine::SetControlRotation(ctrl, ue_wrap::FRotator{pitch, yaw, 0.f});
        const bool ok = ctrl != nullptr;
        char b[64];
        std::snprintf(b, sizeof(b), "%s yaw %.0f pitch %.0f", verb.c_str(), yaw, pitch);
        Answer(id, ok, b);
    } else if (verb == "around") {
        // around <radius cm> [class text]: the actors near this player, nearest first, to <tag>_around.txt.
        float radius = 500.f;
        std::string sub;
        in >> radius >> sub;
        const auto v = ActorsAround(Lower(sub), radius);
        std::ostringstream o;
        for (const Near& a : v) {
            char b[96];
            std::snprintf(b, sizeof(b), "%7.0f  (%.0f %.0f %.0f)  ", a.dist, a.at.X, a.at.Y, a.at.Z);
            o << b << Narrow(R::ClassNameOf(a.obj)) << " " << Narrow(R::ToString(R::NameOf(a.obj))) << "\r\n";
        }
        WriteText(PathOf(L"_around.txt"), o.str());
        Answer(id, true, "around " + std::to_string(v.size()) + " actors");
    } else if (verb == "key") {
        // key <target>: the portable identity peers name this actor by, readable and on the wire.
        std::string t;
        in >> t;
        void* obj = Target(t);
        if (!obj) { Answer(id, false, "key: no target " + t); return; }
        Answer(id, true, "key " + t + " = '" + Narrow(coop::element::PortableIdentity(obj)) + "' wire '" +
                             Narrow(coop::element::PortableWireKey(obj)) + "'");
    } else if (verb == "spawn") {
        // spawn <Class> <x> <y> <z>: the class's actor at that place (the game's own spawn; a host's is broadcast
        // as any runtime birth is).
        // spawn <Class> near <target> [dist]: beside the target, on this player's side, 50 cm up.
        std::string cls, word;
        float x = 0, y = 0, z = 0;
        in >> cls >> word;
        if (word == "near") {
            std::string t;
            float dist = 100.f, yaw = 0.f;
            in >> t;
            if (!(in >> dist)) dist = 100.f;
            void* obj = Target(t);
            ue_wrap::FVector st{};
            if (!obj || !local || !StandNear(local, obj, dist, st, yaw)) { Answer(id, false, "spawn: no target " + t + ", or no floor beside it"); return; }
            x = st.X; y = st.Y; z = st.Z - 45.f;
        } else {
            x = static_cast<float>(std::atof(word.c_str()));
            in >> y >> z;
        }
        void* c = R::FindClass(Wide(cls).c_str());
        if (!c) { Answer(id, false, "spawn: no class " + cls); return; }
        void* a = ue_wrap::engine::SpawnActor(c, ue_wrap::FVector{x, y, z});
        Answer(id, a != nullptr, "spawn " + cls + (a ? " -> " + Narrow(R::ToString(R::NameOf(a))) : std::string(" failed")));
    } else if (verb == "grabintent") {
        // grabintent <target>: send the host the trash grab intent for that mirror's eid, as the E-press
        // on it does (a client's resting ball or pile, when the aim cannot reach it).
        std::string t;
        in >> t;
        void* obj = Target(t);
        const coop::element::ElementId eid = obj ? coop::remote_prop::ResolveMirrorEidByActor(obj)
                                                 : coop::element::kInvalidId;
        coop::net::Session* s = &harness::session_runtime::Session();
        if (eid == coop::element::kInvalidId || !s) { Answer(id, false, "grabintent: no mirror eid for " + t); return; }
        coop::trash_channel::SendGrabIntent(*s, static_cast<uint32_t>(eid));
        Answer(id, true, "grabintent eid " + std::to_string(eid));
    } else if (verb == "throwintent") {
        // throwintent: let go of the clump this client carries (the E-press while carrying).
        coop::net::Session* s = &harness::session_runtime::Session();
        const coop::element::ElementId carry = coop::trash_channel::ClientCarryEid();
        if (!s || carry == coop::element::kInvalidId) { Answer(id, false, "throwintent: carrying nothing"); return; }
        coop::trash_channel::SendThrowIntent(*s, static_cast<uint32_t>(carry), coop::net::throw_mode::kRelease,
                                             ue_wrap::FVector{});
        Answer(id, true, "throwintent eid " + std::to_string(carry));
    } else if (verb == "resync") {
        coop::session_manager::RequestResync();
        Answer(id, true, "resync asked");
    } else if (verb == "worlds") {
        // Every UWorld and level with its WorldSettings, and the root-set holders of a dead one,
        // into the log under the given tag: the instrument for a world a travel left behind.
        const std::string tag = rest.empty() ? std::string("lab") : rest;
        UE_LOGI("[LAB] ---- worlds %s ----", tag.c_str());
        harness::autotest::WorldCensusNowGT(tag.c_str());
        Answer(id, true, "worlds " + tag + " (see log: reloadchurn[" + tag + " c0])");
    } else if (verb == "gc") {
        Answer(id, harness::autotest::ForceCollectionNowGT(), "gc armed");
    } else if (verb == "pins") {
        const size_t n = ue_wrap::GcPin::ReportWorldScopedPins("lab");
        Answer(id, true, "world-scoped pins " + std::to_string(n) + " of " +
                             std::to_string(ue_wrap::GcPin::Outstanding()));
    } else if (verb == "press") {
        // press <target> <action> [seconds]: the action's press event now, its release after the hold.
        std::string t, action;
        float secs = 0.15f;
        in >> t >> action;
        if (!(in >> secs)) secs = 0.15f;
        void* obj = Target(t);
        ActionPair ap = ActionEvents(obj, action);
        std::string how = "bindings";
        if (ap.press.empty() && ap.release.empty()) {
            const auto ev = InputEvents(obj, false, action);
            if (!ev.empty()) ap.press.push_back(ev[0]);
            if (ev.size() > 1) ap.release.push_back(ev[1]);
            how = "node order";
        }
        if (ap.press.empty() && ap.release.empty()) { Answer(id, false, "press: no InpActEvt_" + action + " on " + t); return; }
        bool ok = true;
        for (void* fn : ap.press) ok = CallInput(obj, fn, false, 0.f) && ok;
        for (void* fn : ap.release) {
            Timed tm;
            tm.obj = obj; tm.idx = R::InternalIndexOf(obj); tm.fn = fn; tm.axis = false;
            tm.until = Clock::now() + std::chrono::milliseconds(static_cast<int>(secs * 1000.f));
            tm.what = "press " + t + " " + action;
            g_timed.push_back(tm);
        }
        Answer(id, ok, "press " + t + " " + action + " (" + how + ": " + std::to_string(ap.press.size()) + " press, " +
                           std::to_string(ap.release.size()) + " release, held " + std::to_string(secs) + " s)");
    } else if (verb == "axis") {
        // axis <target> <name> <value> <seconds>: the axis event every tick with that value, then 0.
        std::string t, name;
        float value = 0.f, secs = 1.f;
        in >> t >> name >> value;
        if (!(in >> secs)) secs = 1.f;
        void* obj = Target(t);
        const auto ev = InputEvents(obj, true, name);
        if (ev.empty()) { Answer(id, false, "axis: no InpAxisEvt_" + name + " on " + t); return; }
        Timed tm;
        tm.obj = obj; tm.idx = R::InternalIndexOf(obj); tm.fn = ev[0]; tm.axis = true; tm.value = value;
        tm.until = Clock::now() + std::chrono::milliseconds(static_cast<int>(secs * 1000.f));
        tm.what = "axis " + t + " " + name;
        g_timed.push_back(tm);
        Answer(id, true, "axis " + t + " " + name + " = " + std::to_string(value) + " for " + std::to_string(secs) + " s");
    } else if (verb == "release") {
        const size_t n = g_timed.size();
        ReleaseAll();
        Answer(id, true, "released " + std::to_string(n) + " input(s)");
    } else if (verb == "daytime") {
        // daytime <hour 0..24>: the within-day clock set there (on the host it is the shared clock).
        float hour = 12.f;
        in >> hour;
        float total = 0.f, day = 0.f, scale = 0.f, maxTime = 0.f;
        namespace DNC = ue_wrap::daynightcycle;
        if (!DNC::ReadClock(total, day, scale) || !DNC::ReadMaxTime(maxTime) || maxTime <= 0.f) {
            Answer(id, false, "daytime: the day-night cycle is not resolved");
            return;
        }
        const float want = maxTime * std::fmod((std::max)(0.f, hour), 24.f) / 24.f;
        DNC::ApplyClock(total + (want - day), want);
        Answer(id, true, "daytime " + std::to_string(hour) + " (day " + std::to_string(day) + " -> " +
                             std::to_string(want) + " of " + std::to_string(maxTime) + ")");
    } else if (verb == "vitals") {
        // vitals name=value ... (food, sleep, health, ...): the player's vital rows written.
        std::string kv, done;
        bool ok = true;
        while (in >> kv) {
            const size_t eq = kv.find('=');
            V::Field f{};
            if (eq == std::string::npos || !FieldByName(kv.substr(0, eq), f) ||
                !V::Write(f, std::strtof(kv.c_str() + eq + 1, nullptr))) { ok = false; done += " !" + kv; continue; }
            done += " " + kv;
        }
        Answer(id, ok, "vitals" + done);
    } else if (verb == "keepfed") {
        std::string v;
        in >> v;
        g_keepFed = (v != "0" && v != "off");
        if (g_keepFed) Feed();
        Answer(id, true, std::string("keepfed ") + (g_keepFed ? "on" : "off"));
    } else if (verb == "goto") {
        // goto <target> [dist] [dz]: stand `dist` cm in front of the target (along its own forward, where a
        // switch, a screen or a door faces), `dz` cm above its origin, and look at it -- the spot a player
        // stands to use it. A negative dist stands behind it.
        std::string t;
        in >> t;
        float dist = 90.f, dz = 0.f;
        if (!(in >> dist)) dist = 90.f;
        if (!(in >> dz)) dz = 0.f;
        void* obj = Target(t);
        ue_wrap::FVector at{};
        ue_wrap::FRotator r{};
        if (!obj || !ue_wrap::engine::TryGetActorLocation(obj, at) || !ue_wrap::engine::TryGetActorRotation(obj, r)) {
            Answer(id, false, "goto: no target " + t);
            return;
        }
        const double rad = r.Yaw * 3.14159265358979 / 180.0;
        const float x = at.X + static_cast<float>(std::cos(rad) * dist);
        const float y = at.Y + static_cast<float>(std::sin(rad) * dist);
        const float z = at.Z + dz;
        const float faceYaw = static_cast<float>(std::atan2(at.Y - y, at.X - x) * 180.0 / 3.14159265358979);
        const bool ok = coop::teleport_client::ApplyLocally({x, y, z, 0.f, faceYaw, 0.f});
        if (void* ctrl = LocalController()) {
            void* local = coop::players::Registry::Get().Local();
            ue_wrap::FVector me{x, y, z};
            if (local) ue_wrap::engine::TryGetActorLocation(local, me);
            const float eyeZ = me.Z + 64.f;
            const float dxy = std::sqrt((at.X - x) * (at.X - x) + (at.Y - y) * (at.Y - y));
            const float pitch = static_cast<float>(std::atan2(at.Z - eyeZ, (std::max)(dxy, 1.f)) * 180.0 / 3.14159265358979);
            ue_wrap::engine::SetControlRotation(ctrl, ue_wrap::FRotator{pitch, faceYaw, 0.f});
        }
        char b[160];
        std::snprintf(b, sizeof(b), "goto %s: stand (%.0f,%.0f,%.0f) facing %.0f", t.c_str(), x, y, z, faceYaw);
        Answer(id, ok, b);
    } else if (verb == "drill") {
        // drill <row|VOTVCOOP_ROW> <value> [<row> <value> ...] | drill <row> off | drill reset: a drill switched on or off
        // inside the running session, without a relaunch. The value is held in the config's runtime layer only (never
        // multivoid.ini, so nothing outlives this copy), the drills' knobs are read again (rearm), and every drill goes back
        // to its first phase, as a session's end sends it. A drill that needs a fresh join still needs a relaunch.
        std::vector<std::pair<std::string, std::string>> sets;
        std::string k, v;
        while (in >> k) {
            if (k == "reset") continue;
            if (!(in >> v)) { Answer(id, false, "drill: " + k + " has no value"); return; }
            sets.emplace_back(k, v);
        }
        std::string said;
        for (auto& [key, val] : sets) {
            std::string row = key;
            if (row.rfind("VOTVCOOP_", 0) == 0) row = Lower(row.substr(9));   // the environment twin's name
            const coop::config_registry::Row* r = coop::config_registry::FindRow(row.c_str());
            if (!r) { Answer(id, false, "drill: no row " + key); return; }
            if (val == "off" || val == "-") coop::config::internal::RuntimeLayerDrop(r);
            else coop::config::internal::RuntimeLayerPut(r, val);
            said += " " + row + "=" + val;
        }
        coop::dev::rearm::Bump();
        coop::dev::dev_lanes::EndSession();
        coop::dev::dev_lanes::RearmSelftests();
        Answer(id, true, "drill:" + (said.empty() ? std::string(" reset") : said) + " -- every drill at its first phase");
    } else if (verb == "dilation") {
        // dilation [x]: the world's global time dilation (the engine's own, as the game's sleep sets 20), for a drill that
        // waits on game time -- growth, drying, a day's turn. 1 is normal; without x, the current value. Each peer has its
        // own world: set it on the host for the host's clock and growth (the clients follow the host's clock samples).
        float x = 0.f;
        if (in >> x) {
            if (x <= 0.f || x > 50.f) { Answer(id, false, "dilation: 0 < x <= 50"); return; }
            if (!ue_wrap::sleep::SetGlobalTimeDilation(x)) { Answer(id, false, "dilation: the call did not run"); return; }
        }
        Answer(id, true, "dilation = " + std::to_string(ue_wrap::sleep::GetGlobalTimeDilation()));
    } else if (verb == "count") {
        // count <class text>: how many live actors of a class holding the text this world has (what near: and
        // around read), for a scenario's "both peers have as many".
        std::string sub;
        in >> sub;
        Answer(id, true, "count " + sub + " = " + std::to_string(ActorsAround(Lower(sub), 1e9f).size()));
    } else if (verb == "use" || verb == "apply") {
        // use <target> [component|-] [action]: the use key's verb on it as the E press makes it -- the target's own
        // actionOptionIndex with this player, a hit on the named component (the root when none) and the action (0).
        // apply <target> [component|-]: the held thing used on it -- the target's playerUsedOn with the hand's actor.
        std::string t, comp;
        int action = 0;
        in >> t >> comp;
        if (!(in >> action)) action = 0;
        void* obj = Target(t);
        if (!obj || !local) { Answer(id, false, verb + ": no target " + t); return; }
        void* c = nullptr;
        if (!comp.empty() && comp != "-") {
            c = ComponentNamed(obj, comp);
            if (!c) { Answer(id, false, verb + ": no component " + comp + " in " + t); return; }
        }
        std::string out;
        const bool ok = verb == "use"
            ? PlayerVerb(obj, c, L"actionOptionIndex", local, action, nullptr, out)
            : PlayerVerb(obj, c, L"playerUsedOn", local, -1, coop::director::Grabbing(local), out);
        Answer(id, ok, verb + " " + t + (c ? "/" + comp : std::string()) + ":" + out);
    } else if (verb == "grab") {
        // grab <target>: the director's aimed grab -- the camera turned until this player's own trace takes it, then
        // the use key's grab chain. The player must stand within reach (tp near first). Answered when it ends.
        std::string t;
        in >> t;
        void* obj = Target(t);
        if (!obj || !local) { Answer(id, false, "grab: no target " + t); return; }
        if (g_grab.grab) { Answer(id, false, "grab: another grab is in flight"); return; }
        g_grab.id = id;
        g_grab.grab = std::make_unique<coop::director::AimedGrab>(local, obj);
        g_grab.until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    } else if (verb == "hold") {
        // hold <target>: the player's own "Hold Object" with the target -- the hand's pickup (the R key), the world
        // actor gone and the item in the hotbar hand. On a client a shared prop's pickup is asked of the host, so the
        // hand fills a round trip later: read hand:<slot> or the state file's hand line after it.
        std::string t;
        in >> t;
        void* obj = Target(t);
        bool collected = false;
        const bool ok = obj && local && ue_wrap::engine::CallMainPlayerHoldObject(local, obj, collected);
        Answer(id, ok, "hold " + t + (collected ? ": collected" : ": not collected (yet)"));
    } else if (verb == "drop") {
        // drop: the hand lets go of what it holds, by the player's own dropGrabObject.
        const bool ok = local && coop::director::CallOnPlayer(local, L"dropGrabObject");
        Answer(id, ok, "drop");
    } else if (verb == "route" || verb == "ring") {
        // route <x> <y> <z>: the director's NavMesh route from this player to the point -- its points, its flat
        // length and how far its end stops short of the point (a partial route ends short of it).
        // ring <radius cm> [count] [x y]: which of `count` points of a ring about (x, y) -- this player when not
        // given -- a route reaches, by the drills' own rule (ReachableStandpoints, reach 100 cm). Both for finding a
        // lab spot where a walking drill's NavMesh holds.
        ue_wrap::FVector me{};
        if (!local || !ue_wrap::engine::TryGetActorLocation(local, me)) { Answer(id, false, verb + ": no player"); return; }
        auto flat = [](const ue_wrap::FVector& a, const ue_wrap::FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); };
        std::ostringstream o;
        if (verb == "route") {
            ue_wrap::FVector to{};
            in >> to.X >> to.Y >> to.Z;
            std::vector<ue_wrap::FVector> r;
            if (!coop::director::RouteFrom(local, me, to, &r) || r.empty()) { Answer(id, false, "route: none"); return; }
            float len = 0.f;
            for (size_t k = 1; k < r.size(); ++k) len += flat(r[k - 1], r[k]);
            char b[160];
            std::snprintf(b, sizeof(b), "route: %zu points, %.0f cm, ends %.0f cm from the point at (%.0f %.0f %.0f)",
                          r.size(), len, flat(r.back(), to), r.back().X, r.back().Y, r.back().Z);
            o << b;
        } else {
            float radius = 1000.f, x = me.X, y = me.Y;
            int count = 16;
            in >> radius;
            if (!(in >> count)) count = 16;
            if (in >> x) in >> y;
            const auto pts = coop::director::ReachableStandpoints(local, {x, y, me.Z}, radius, count, 100.f);
            char b[120];
            std::snprintf(b, sizeof(b), "ring %.0f about (%.0f %.0f): %zu of %d reached", radius, x, y, pts.size(), count);
            o << b;
            for (const auto& q : pts) {
                std::snprintf(b, sizeof(b), " (%.0f %.0f %.0f)", q.X, q.Y, q.Z);
                o << b;
            }
        }
        Answer(id, true, o.str());
    } else if (verb == "funcs") {
        // funcs <target|Class> [text]: the functions (and their parameters) into <tag>_funcs.txt.
        std::string t, filter;
        in >> t >> filter;
        const std::string r = Funcs(t, filter);
        Answer(id, !r.empty(), r.empty() ? "funcs: no class " + t : "funcs " + t + ": " + r);
    } else if (verb == "cam") {
        // cam <target> [dist] [yaw] [height]: a camera `dist` cm from the target at compass `yaw`, `height` up,
        // looking at it, and the view handed to it. cam off: the view back to the player.
        std::string t;
        in >> t;
        void* ctrl = LocalController();
        if (t == "off") {
            const bool ok = SetViewTarget(ctrl, coop::players::Registry::Get().Local());
            Answer(id, ok, "cam off");
            return;
        }
        float dist = 300.f, yaw = 0.f, height = 100.f;
        if (!(in >> dist)) dist = 300.f;
        if (!(in >> yaw)) yaw = 0.f;
        if (!(in >> height)) height = 100.f;
        void* obj = Target(t);
        ue_wrap::FVector at{};
        if (!obj || !ue_wrap::engine::TryGetActorLocation(obj, at)) { Answer(id, false, "cam: no target " + t); return; }
        const double rad = yaw * 3.14159265358979 / 180.0;
        const ue_wrap::FVector eye{at.X + static_cast<float>(std::cos(rad) * dist),
                                   at.Y + static_cast<float>(std::sin(rad) * dist), at.Z + height};
        if (!g_cam || !R::IsLiveByIndex(g_cam, g_camIdx)) {
            g_cam = ue_wrap::engine::SpawnActor(R::FindClass(L"CameraActor"), eye, false);
            g_camIdx = g_cam ? R::InternalIndexOf(g_cam) : -1;
        }
        if (!g_cam) { Answer(id, false, "cam: no CameraActor"); return; }
        const float dx = at.X - eye.X, dy = at.Y - eye.Y, dz = at.Z - eye.Z;
        const ue_wrap::FRotator rot{static_cast<float>(std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.0 / 3.14159265358979),
                                    static_cast<float>(std::atan2(dy, dx) * 180.0 / 3.14159265358979), 0.f};
        ue_wrap::engine::SetActorLocation(g_cam, eye);
        ue_wrap::engine::SetActorRotation(g_cam, rot);
        const bool ok = SetViewTarget(ctrl, g_cam);
        char b[160];
        std::snprintf(b, sizeof(b), "cam %s at (%.0f,%.0f,%.0f) yaw %.0f pitch %.0f", t.c_str(), eye.X, eye.Y, eye.Z, rot.Yaw, rot.Pitch);
        Answer(id, ok, b);
    } else if (verb == "log") {
        UE_LOGI("[LAB] ---- %s ----", rest.c_str());
        Answer(id, true, "log");
    } else if (verb == "window") {
        // window show|hide: a copy launched hidden brought onto the screen (still not activated) for a
        // look, or parked off it again.
        std::string v;
        in >> v;
        const bool ok = lab_window::SetHidden(v != "show");
        Answer(id, ok, ok ? "window " + v : std::string("window: this copy was not launched hidden"));
    } else if (verb == "widgets") {
        // widgets [target|-] [text]: the live user widgets, or the widgets inside one, to <tag>_widgets.txt.
        std::string t, filter;
        in >> t >> filter;
        void* root = nullptr;
        if (!t.empty() && t != "-") {
            root = Target(t);
            if (!root) { Answer(id, false, "widgets: no target " + t); return; }
        }
        Answer(id, true, "widgets " + t + ": " + Widgets(root, Lower(filter)));
    } else if (verb == "click") {
        // click <target> [delegate]: the button's OnClicked (or the named delegate: OnPressed, OnReleased,
        // OnHovered) broadcast, as the mouse's click on it does.
        std::string t, del;
        in >> t >> del;
        if (del.empty()) del = "OnClicked";
        void* obj = Target(t);
        if (!obj) { Answer(id, false, "click: no target " + t); return; }
        std::string out;
        const int ran = Broadcast(obj, Wide(del), out);
        Answer(id, ran > 0, "click " + t + " " + del + ": " + std::to_string((std::max)(ran, 0)) + " handler(s)" + out);
    } else if (verb == "focus") {
        // focus <target>: the widget takes Slate's keyboard focus (an edit box before `type`).
        std::string t;
        in >> t;
        void* obj = Target(t);
        Answer(id, obj && CallNoParams(obj, L"SetKeyboardFocus"), "focus " + t);
    } else if (verb == "type") {
        // type <text>: the characters, and {enter} {tab} {back} {esc} {up} {down} {left} {right} {space}
        // {del} as keys, posted to the game window for the focused widget, as the keyboard sends them.
        const int n = lab_window::PostText(Wide(rest));
        Answer(id, n > 0, "type: " + std::to_string(n) + " message(s) posted");
    } else if (verb == "drag") {
        // drag <target> <action> <seconds> <axis>=<value> ...: the action held for the seconds while each
        // named axis is pushed for the same time -- a button held while the mouse moves (a brush across
        // a window, a lever pulled through its arc).
        std::string t, action, kv;
        float secs = 1.f;
        in >> t >> action >> secs;
        void* obj = Target(t);
        if (!obj) { Answer(id, false, "drag: no target " + t); return; }
        const ActionPair ap = ActionEvents(obj, action);
        if (ap.press.empty()) { Answer(id, false, "drag: no press event for " + action + " on " + t); return; }
        const auto until = Clock::now() + std::chrono::milliseconds(static_cast<int>(secs * 1000.f));
        std::string axes;
        while (in >> kv) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const auto ev = InputEvents(obj, true, kv.substr(0, eq));
            if (ev.empty()) { Answer(id, false, "drag: no InpAxisEvt_" + kv.substr(0, eq) + " on " + t); return; }
            Timed tm;
            tm.obj = obj; tm.idx = R::InternalIndexOf(obj); tm.fn = ev[0]; tm.axis = true;
            tm.value = std::strtof(kv.c_str() + eq + 1, nullptr);
            tm.until = until;
            tm.what = "drag axis " + kv;
            g_timed.push_back(tm);
            axes += " " + kv;
        }
        bool ok = true;
        for (void* fn : ap.press) ok = CallInput(obj, fn, false, 0.f) && ok;  // every bound row, as press does
        for (void* fn : ap.release) {
            Timed tm;
            tm.obj = obj; tm.idx = R::InternalIndexOf(obj); tm.fn = fn; tm.axis = false;
            tm.until = until;
            tm.what = "drag " + action;
            g_timed.push_back(tm);
        }
        Answer(id, ok, "drag " + t + " " + action + " for " + std::to_string(secs) + " s with" + (axes.empty() ? " no axis" : axes));
    } else if (verb == "watch") {
        // watch <name> <target> <property|@loc|@rot|@live|@class>: a line "watch <name> <value>" in the
        // state file every second, until unwatch.
        Watch w;
        in >> w.name >> w.target >> w.prop;
        if (w.name.empty() || w.target.empty() || w.prop.empty()) { Answer(id, false, "watch <name> <target> <prop>"); return; }
        g_watches.erase(std::remove_if(g_watches.begin(), g_watches.end(), [&](const Watch& x) { return x.name == w.name; }),
                        g_watches.end());
        g_watches.push_back(w);
        Answer(id, true, "watch " + w.name + " = " + WatchValue(g_watches.back()));
    } else if (verb == "unwatch") {
        std::string name;
        in >> name;
        const size_t before = g_watches.size();
        if (name.empty() || name == "all") g_watches.clear();
        else g_watches.erase(std::remove_if(g_watches.begin(), g_watches.end(), [&](const Watch& x) { return x.name == name; }),
                             g_watches.end());
        Answer(id, true, "unwatch " + std::to_string(before - g_watches.size()));
    } else {
        Answer(id, false, "unknown verb '" + verb + "'");
    }
}


// ---- the script queue: a command file is a script, run in order; `wait` and `waitworld` pause it --
struct Pending { long long id; std::string verb, rest; };
std::deque<Pending> g_queue;
Clock::time_point g_resumeAt{};
Clock::time_point g_waitDeadline{};  // a waitworld's timeout, set when it starts
bool g_waitingWorld = false;

std::string CurrentWorldName() {
    void* w = ue_wrap::world_identity::CurrentWorld();
    return w ? Lower(Narrow(R::ToString(R::NameOf(w)))) : std::string();
}

void RunQueue(Clock::time_point now) {
    while (!g_queue.empty() && now >= g_resumeAt) {
        Pending& p = g_queue.front();
        if (p.verb == "wait") {
            const float secs = std::strtof(p.rest.c_str(), nullptr);
            g_resumeAt = now + std::chrono::milliseconds(static_cast<int>(secs * 1000.f));
            Answer(p.id, true, "wait " + p.rest);
            g_queue.pop_front();
            continue;
        }
        if (p.verb == "waitworld") {
            // waitworld <name text> [timeout s]: hold the script until the current world's name holds it.
            std::istringstream in(p.rest);
            std::string want;
            float timeout = 120.f;
            in >> want;
            if (!(in >> timeout)) timeout = 120.f;
            if (!g_waitingWorld) {
                g_waitingWorld = true;
                g_waitDeadline = now + std::chrono::milliseconds(static_cast<int>(timeout * 1000.f));
            }
            const std::string cur = CurrentWorldName();
            const bool there = !cur.empty() && cur.find(Lower(want)) != std::string::npos;
            if (!there && now < g_waitDeadline) return;  // still waiting: nothing after it runs
            g_waitingWorld = false;
            Answer(p.id, there, "waitworld " + want + (there ? " reached" : " TIMED OUT, world '" + cur + "'"));
            g_queue.pop_front();
            continue;
        }
        const Pending cur = p;
        g_queue.pop_front();
        Run(cur.id, cur.verb, cur.rest);
    }
}

void Poll() {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!::GetFileAttributesExW(PathOf(L"_cmd.txt").c_str(), GetFileExInfoStandard, &a)) return;
    if (::CompareFileTime(&a.ftLastWriteTime, &g_lastWrite) == 0) return;
    std::ifstream f(PathOf(L"_cmd.txt"));
    if (!f.is_open()) return;   // mid-write by the sender: the next poll reads it whole
    g_lastWrite = a.ftLastWriteTime;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        long long id = 0;
        std::string verb;
        if (!(ls >> id >> verb) || id <= g_lastId) continue;
        g_lastId = id;
        std::string rest;
        std::getline(ls, rest);
        const size_t b = rest.find_first_not_of(' ');
        rest = b == std::string::npos ? std::string() : rest.substr(b);
        g_queue.push_back({id, verb, rest});
    }
}

void WriteState() {
    std::ostringstream o;
    void* world = ue_wrap::world_identity::CurrentWorld();
    o << "time " << ::GetTickCount64() << "\r\n";
    {
        const auto now = std::chrono::steady_clock::now();
        const double span = std::chrono::duration<double>(now - g_lastStateAt).count();
        char b[96];
        std::snprintf(b, sizeof(b), "fps %.1f worst %.0f ms\r\n", span > 0 ? g_frames / span : 0.0, g_worstMs);
        o << b;
        g_frames = 0;
        g_worstMs = 0.0;
        g_lastStateAt = now;
    }
    o << "world " << (world ? Narrow(R::ToString(R::NameOf(world))) : std::string("-")) << " gen "
      << ue_wrap::world_identity::Generation() << "\r\n";
    o << "dimension " << (coop::dimension_travel::InDimension() ? 1 : 0) << "\r\n";
    coop::net::Session& s = harness::session_runtime::Session();
    o << "session running=" << (s.running() ? 1 : 0) << " connected=" << (s.connected() ? 1 : 0)
      << " role=" << (s.role() == coop::net::Role::Host ? "host" : "client") << "\r\n";
    {
        namespace V = ue_wrap::vitals;
        o << "vitals";
        for (uint8_t i = 0; i < static_cast<uint8_t>(V::Field::Count); ++i) {
            float v = 0.f;
            const V::Field f = static_cast<V::Field>(i);
            if (V::Read(f, &v)) { char b[48]; std::snprintf(b, sizeof(b), " %s=%.1f", V::RowOf(f).name, v); o << b; }
        }
        o << "\r\n";
    }
    if (void* local = coop::players::Registry::Get().Local()) {
        ue_wrap::FVector l{};
        ue_wrap::engine::TryGetActorLocation(local, l);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "local %.0f %.0f %.0f\r\n", l.X, l.Y, l.Z);
        o << buf;
    } else {
        o << "local -\r\n";
    }
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        auto& p = coop::puppet_drive::Puppet(slot);
        if (!p.valid()) continue;
        ue_wrap::FVector l{};
        p.TryGetLocation(l);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "puppet %d %.0f %.0f %.0f\r\n", slot, l.X, l.Y, l.Z);
        o << buf;
    }
    // What each body holds: this player's hotbar hand, and each peer's as this peer shows it (a compare pair).
    if (void* h = coop::hand_item::LocalHandActor()) o << "hand " << Narrow(R::ClassNameOf(h)) << "\r\n";
    else o << "hand -\r\n";
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        const std::wstring held = coop::hand_item::HeldItem(static_cast<uint8_t>(slot));
        // The hand generation, and on a remote slot the generation of its placed mirror (0: none matches yet).
        if (!held.empty())
            o << "handof " << slot << " " << Narrow(held) << " gen=" << coop::hand_item::Generation(static_cast<uint8_t>(slot))
              << " mirror=" << coop::hand_item::MirrorGeneration(static_cast<uint8_t>(slot)) << "\r\n";
    }
    o << SmoothLines();
    for (Watch& w : g_watches) o << "watch " << w.name << " " << WatchValue(w) << "\r\n";
    o << "window " << (lab_window::Hidden() ? "hidden" : "shown") << "\r\n";
    const std::wstring tmp = PathOf(L"_state.tmp");
    if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        const std::string t = o.str();
        std::fwrite(t.data(), 1, t.size(), f);
        std::fclose(f);
        ::MoveFileExW(tmp.c_str(), PathOf(L"_state.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

}  // namespace

void Tick() {
    if (!g_init) {
        g_init = true;
        g_dir = Env(L"VOTVCOOP_LAB_DIR");
        g_tag = Env(L"VOTVCOOP_LAB_TAG");
        g_on = !g_dir.empty() && !g_tag.empty();
        if (g_on) {
            // Commands already in the file belong to an earlier launch.
            std::ifstream f(PathOf(L"_cmd.txt"));
            std::string line;
            while (std::getline(f, line)) {
                const long long id = std::atoll(line.c_str());
                if (id > g_lastId) g_lastId = id;
            }
            UE_LOGI("[LAB] the lab channel is on: %ls\\%ls_cmd.txt (commands after #%lld)", g_dir.c_str(),
                    g_tag.c_str(), g_lastId);
        }
    }
    if (!g_on) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_lastFrame.time_since_epoch().count() != 0) {
        const double ms = std::chrono::duration<double, std::milli>(now - g_lastFrame).count();
        if (ms > g_worstMs) g_worstMs = ms;
    }
    g_lastFrame = now;
    ++g_frames;
    if (now >= g_nextPoll) { g_nextPoll = now + std::chrono::milliseconds(200); Poll(); }
    RunQueue(now);
    TickTimed(now);
    TickGrab(now);
    SamplePuppets(now);
    if (g_keepFed && now >= g_nextFeed) { g_nextFeed = now + std::chrono::seconds(5); Feed(); }
    if (now >= g_nextState) { g_nextState = now + std::chrono::seconds(1); WriteState(); }
}

}  // namespace coop::dev::lab_cmd
