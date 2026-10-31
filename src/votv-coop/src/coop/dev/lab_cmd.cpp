// coop/dev/lab_cmd.cpp -- see coop/dev/lab_cmd.h.

#include "coop/dev/lab_cmd.h"

#include "coop/element/portable_identity.h"

#include "coop/net/session.h"
#include "coop/session/session_manager.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/teleport_client.h"
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
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/gc_pin.h"       // ReportWorldScopedPins (pins)
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/world/daynightcycle.h"  // daytime

#include <windows.h>

#include <cctype>
#include <deque>
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
        return off < 0 ? nullptr : *reinterpret_cast<void**>(static_cast<char*>(base) + off);
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
    if (t.rfind("near:", 0) == 0) {  // the nearest live actor whose class name holds the text
        const auto v = ActorsAround(Lower(t.substr(5)), 1e9f);
        return v.empty() ? nullptr : v.front().obj;
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
struct ActionPair { void* press = nullptr; void* release = nullptr; int rows = 0; };

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
                if (edge == 0 && !out.press) out.press = fn;
                else if (edge == 1 && !out.release) out.release = fn;
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

void Run(long long id, const std::string& verb, const std::string& rest) {
    std::istringstream in(rest);
    void* local = coop::players::Registry::Get().Local();
    if (verb == "tp") {
        float x = 0, y = 0, z = 0, yaw = 0;
        in >> x >> y >> z >> yaw;
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
        std::string cls;
        float x = 0, y = 0, z = 0;
        in >> cls >> x >> y >> z;
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
        if (!ap.press && !ap.release) {
            const auto ev = InputEvents(obj, false, action);
            if (!ev.empty()) ap.press = ev[0];
            if (ev.size() > 1) ap.release = ev[1];
            how = "node order";
        }
        if (!ap.press && !ap.release) { Answer(id, false, "press: no InpActEvt_" + action + " on " + t); return; }
        const bool ok = ap.press ? CallInput(obj, ap.press, false, 0.f) : true;
        if (ap.release) {
            Timed tm;
            tm.obj = obj; tm.idx = R::InternalIndexOf(obj); tm.fn = ap.release; tm.axis = false;
            tm.until = Clock::now() + std::chrono::milliseconds(static_cast<int>(secs * 1000.f));
            tm.what = "press " + t + " " + action;
            g_timed.push_back(tm);
        }
        Answer(id, ok, "press " + t + " " + action + " (" + how + ": press " + (ap.press ? "yes" : "no") +
                           ", release " + (ap.release ? "yes" : "no") + ", held " + std::to_string(secs) + " s)");
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
    if (g_keepFed && now >= g_nextFeed) { g_nextFeed = now + std::chrono::seconds(5); Feed(); }
    if (now >= g_nextState) { g_nextState = now + std::chrono::seconds(1); WriteState(); }
}

}  // namespace coop::dev::lab_cmd
