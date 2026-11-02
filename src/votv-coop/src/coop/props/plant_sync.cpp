// coop/props/plant_sync.cpp -- see coop/props/plant_sync.h.

#include "coop/props/plant_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::plant_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};
coop::blob_chunks::Assembler g_asm;
uint32_t g_sequence = 0;
uint32_t g_world = 0;

bool Active(coop::net::Session* s) { return s && s->running() && s->connected(); }
bool IsHost(coop::net::Session* s) { return s && s->role() == coop::net::Role::Host; }
uint32_t Next(uint32_t& v) { if (!++v) ++v; return v; }

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(c > 0 && c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// ---- a plant, read and written ---------------------------------------------------------------------------

// What travels of one plant: the identity, the place, and the plant's own save fields (getData's grow,
// fertilizer, water, fruitGrow and dryness, with plant and AttachParent), plus Amount -- whether a bush has
// berries to pick, which no record carries.
struct Rec {
    std::string id;   // the key, or, for a key the save gave several plants, the key and the place (Ident)
    std::string key, cls, plant, attach;
    float loc[3] = {0, 0, 0}, rot[3] = {0, 0, 0}, scale[3] = {1, 1, 1};
    float grow = 0, fert = 0, water = 0, fruit = 0, dry = 0;
    int32_t amount = 0;
};

struct Offs {
    int32_t key = -1, plant = -1, attach = -1, grow = -1, fert = -1, water = -1, fruit = -1, dry = -1, amount = -1,
            data = -1, grewOff = -1;
    uint8_t grewMask = 0;
    bool ok = false;
};
std::unordered_map<void*, Offs> g_offs;  // per class

const Offs& OffsFor(void* cls) {
    auto it = g_offs.find(cls);
    if (it != g_offs.end()) return it->second;
    Offs o;
    o.key    = R::FindPropertyOffset(cls, L"Key");
    o.plant  = R::FindPropertyOffset(cls, L"plant");
    o.attach = R::FindPropertyOffset(cls, L"AttachParent");
    o.grow   = R::FindPropertyOffset(cls, L"grow");
    o.fert   = R::FindPropertyOffset(cls, L"fertilizer");
    o.water  = R::FindPropertyOffset(cls, L"Water");
    o.fruit  = R::FindPropertyOffset(cls, L"fruitGrow");
    o.dry    = R::FindPropertyOffset(cls, L"dryness");
    o.amount = R::FindPropertyOffset(cls, L"Amount");
    o.data   = R::FindPropertyOffset(cls, L"Data");
    R::FindBoolProperty(cls, L"grew", o.grewOff, o.grewMask);
    o.ok = o.key >= 0 && o.plant >= 0 && o.attach >= 0 && o.grow >= 0 && o.fert >= 0 && o.water >= 0 && o.fruit >= 0 &&
           o.dry >= 0 && o.amount >= 0;
    if (!o.ok)
        UE_LOGW("plant_sync: a plant class (%ls) lacks a field this lane reads -- its plants stay local",
                R::ToString(R::NameOf(cls)).c_str());
    return g_offs.emplace(cls, o).first->second;
}

template <class T> T Get(void* a, int32_t off) { T v{}; std::memcpy(&v, static_cast<const char*>(a) + off, sizeof v); return v; }
template <class T> void Put(void* a, int32_t off, const T& v) { std::memcpy(static_cast<char*>(a) + off, &v, sizeof v); }

std::string NameAt(void* a, int32_t off) {
    const std::wstring w = R::ToString(Get<R::FName>(a, off));
    return w == L"None" ? std::string() : Narrow(w);
}
void SetNameAt(void* a, int32_t off, const std::string& s) {
    Put(a, off, ue_wrap::fname_utils::StringToFName(Wide(s.empty() ? std::string("None") : s).c_str()));
}

std::string KeyOf(void* a) {
    void* cls = R::ClassOf(a);
    if (!cls) return {};
    const Offs& o = OffsFor(cls);
    return o.ok ? NameAt(a, o.key) : std::string();
}

bool Grew(void* a, const Offs& o) {
    return o.grewOff >= 0 && o.grewMask && (static_cast<const uint8_t*>(a)[o.grewOff] & o.grewMask);
}

// struct_farmPlant (the row the plant loaded into Data): plantType at +0x5C -- 0 a crop, 1 a bush whose fruit
// is picked, 2 a tree that hangs fruit props. Read for the apply's choice of verbs.
int PlantType(void* a, const Offs& o) {
    return o.data >= 0 ? static_cast<int>(Get<uint8_t>(a, o.data + 0x5C)) : -1;
}

bool IsDeadRow(const std::string& row) { return row == "deadtree" || row == "deadbush" || row == "deadgrass"; }

bool Read(void* a, Rec& r) {
    void* cls = R::ClassOf(a);
    if (!cls) return false;
    const Offs& o = OffsFor(cls);
    if (!o.ok) return false;
    r.key = NameAt(a, o.key);
    if (r.key.empty()) return false;   // mid-construction: no identity to address yet
    r.cls = Narrow(R::ToString(R::NameOf(cls)));
    r.plant = NameAt(a, o.plant);
    r.attach = NameAt(a, o.attach);
    ue_wrap::FVector l{};
    ue_wrap::FRotator rt{};
    E::TryGetActorLocation(a, l);
    E::TryGetActorRotation(a, rt);
    const ue_wrap::FVector sc = E::GetActorScale3D(a);
    r.loc[0] = l.X; r.loc[1] = l.Y; r.loc[2] = l.Z;
    r.rot[0] = rt.Pitch; r.rot[1] = rt.Yaw; r.rot[2] = rt.Roll;
    r.scale[0] = sc.X; r.scale[1] = sc.Y; r.scale[2] = sc.Z;
    r.grow = Get<float>(a, o.grow);
    r.fert = Get<float>(a, o.fert);
    r.water = Get<float>(a, o.water);
    r.fruit = Get<float>(a, o.fruit);
    r.dry = Get<float>(a, o.dry);
    r.amount = Get<int32_t>(a, o.amount);
    return true;
}

bool CallFn(void* a, const wchar_t* fn, const wchar_t* boolParam = nullptr, bool value = false) {
    void* f = R::FindDispatchFunctionCached(R::ClassOf(a), fn);
    if (!f) return false;
    ue_wrap::ParamFrame frame(f);
    if (!frame.valid()) return false;
    if (boolParam) {
        const uint8_t b = value ? 1 : 0;
        frame.SetRaw(boolParam, &b, 1);
    }
    return ue_wrap::Call(a, frame);
}

void Destroy(void* a) { CallFn(a, L"K2_DestroyActor"); }

// A pot keeps its plant in `plant` and refuses a second while it holds a live one; a plant arriving from the
// wire is put there, as the pot's own planting does, unless the pot already holds another.
void LinkPot(void* plant, const std::string& attach) {
    if (attach.empty()) return;
    void* pot = coop::prop_key_index::Find(Wide(attach));
    if (!pot) return;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(pot), L"plant");
    if (off < 0) return;
    void* cur = Get<void*>(pot, off);
    if (cur && cur != plant && R::IsLive(cur)) return;
    Put<void*>(pot, off, plant);
}

// The record onto a plant that already stands here. A plant still growing takes every field and re-derives its
// look the way loadData does (upd reloads its row and meshes, updLoc its size); it ripens by its own tick when
// the host's growth reaches the end, as it always has. A grown plant keeps its grown look: upd would put back
// the growing mesh, and processGrown runs grown() (a Stolas herb's lightning) -- only a bush, whose ripe look
// upd itself derives from its fruit, is repainted. A death is the plant's own turnDead, as on the host.
void Apply(void* a, const Rec& r) {
    void* cls = R::ClassOf(a);
    const Offs& o = OffsFor(cls);
    if (!o.ok) return;
    const std::string localRow = NameAt(a, o.plant);
    if (IsDeadRow(r.plant) && !IsDeadRow(localRow)) {
        Put(a, o.water, r.water);
        Put(a, o.fert, r.fert);
        CallFn(a, L"turnDead");   // our call: the client's refusal of its own turnDead lets it through
        return;
    }
    const bool rowChanged = !r.plant.empty() && r.plant != localRow;
    if (rowChanged) SetNameAt(a, o.plant, r.plant);
    SetNameAt(a, o.attach, r.attach);
    Put(a, o.water, r.water);
    Put(a, o.fert, r.fert);
    Put(a, o.fruit, r.fruit);
    Put(a, o.dry, r.dry);
    Put(a, o.amount, r.amount);
    if (!Grew(a, o) || rowChanged) {
        Put(a, o.grow, r.grow);
        CallFn(a, L"upd", L"skipDatatable", false);
        CallFn(a, L"updLoc");
    } else if (PlantType(a, o) == 1) {
        CallFn(a, L"upd", L"skipDatatable", false);
    }
}

// A plant this peer lacks, born as the pot or the hole bears one (the row named before BeginPlay, the key too,
// so its own key is never minted), then loaded as a save loads it: the fields, upd, updLoc and processGrown.
void* Spawn(const Rec& r) {
    void* cls = R::FindClass(Wide(r.cls).c_str());
    void* base = OI::ClassByName(L"growingPlant_C");
    if (!cls || !base || (cls != base && !R::IsDescendantOfAny(cls, &base, 1, 16))) {
        UE_LOGW("plant_sync: no plant class '%s' here -- plant key='%s' not spawned", r.cls.c_str(), r.key.c_str());
        return nullptr;
    }
    const Offs& o = OffsFor(cls);
    if (!o.ok) return nullptr;
    const ue_wrap::FVector loc{r.loc[0], r.loc[1], r.loc[2]};
    const ue_wrap::FRotator rot{r.rot[0], r.rot[1], r.rot[2]};
    void* a = E::BeginDeferredSpawn(cls, loc, rot);
    if (!a) return nullptr;
    SetNameAt(a, o.plant, r.plant);
    SetNameAt(a, o.key, r.key);
    SetNameAt(a, o.attach, r.attach);
    if (!E::FinishDeferredSpawn(a, loc, rot)) return nullptr;
    if (std::fabs(r.scale[0] - 1.f) > 0.001f || std::fabs(r.scale[1] - 1.f) > 0.001f || std::fabs(r.scale[2] - 1.f) > 0.001f)
        E::SetActorScale3D(a, ue_wrap::FVector{r.scale[0], r.scale[1], r.scale[2]});
    Put(a, o.grow, r.grow);
    Put(a, o.fert, r.fert);
    Put(a, o.water, r.water);
    Put(a, o.fruit, r.fruit);
    Put(a, o.dry, r.dry);
    Put(a, o.amount, r.amount);
    const bool grew = r.grow >= 1.f;
    if (o.grewOff >= 0 && o.grewMask) {
        uint8_t& b = static_cast<uint8_t*>(a)[o.grewOff];
        b = grew ? static_cast<uint8_t>(b | o.grewMask) : static_cast<uint8_t>(b & ~o.grewMask);
    }
    CallFn(a, L"upd", L"skipDatatable", false);
    CallFn(a, L"updLoc");
    CallFn(a, L"processGrown", L"skipGrowCheck", grew);
    LinkPot(a, r.attach);
    return a;
}

// ---- the live plants of this world ---------------------------------------------------------------------

std::vector<void*> PlantClasses() {
    static std::vector<void*> s;
    static uint64_t sVer = ~0ull;
    static void* sBase = nullptr;
    void* base = OI::ClassByName(L"growingPlant_C");
    const uint64_t ver = OI::ClassSetVersion();
    if (ver == sVer && base == sBase) return s;
    sVer = ver;
    sBase = base;
    s.clear();
    if (!base) return s;
    struct Ctx { void* base; std::vector<void*>* out; } c{base, &s};
    OI::ForEachClass([](void* ctx, void* cls, void*) {
        auto* x = static_cast<Ctx*>(ctx);
        if (cls == x->base || R::IsDescendantOfAny(cls, &x->base, 1, 16)) x->out->push_back(cls);
    }, &c);
    return s;
}

// A plant of the world that stands. Not "WorldOf == CurrentWorld": the base's garden is in a streamed sublevel,
// whose plants name the sublevel's own UWorld, and a first cut that compared worlds carried 7 plants of 17. A
// client's plants are read only once its world is settled, so a world still loading is never mistaken for one.
bool InThisWorld(void* a, bool host) {
    if (!ue_wrap::world_identity::WorldOf(a)) return false;
    return host || coop::net_pump::IsInAnnouncedWorld(a) || coop::net_pump::IsWorldSettled();
}

std::vector<void*> LivePlants(bool host) {
    std::vector<void*> out;
    for (void* cls : PlantClasses()) {
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    }
    std::vector<void*> here;
    for (void* a : out)
        if (InThisWorld(a, host)) here.push_back(a);
    return here;
}

float Dist(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A plant's identity on the wire. The save does not keep keys unique: the lab's base holds thirteen dead grass
// plants under one key, and a lane keyed on the key alone carried one of them and could have destroyed the
// wrong one on a harvest. A key held once is the identity; a key held by several plants is joined by the
// plant's place on a 10 cm grid -- the same save on every peer, so the same names, and a plant does not move.
std::string Ident(const Rec& r, bool shared) {
    if (!shared) return r.key;
    char b[96];
    std::snprintf(b, sizeof(b), "@%ld,%ld,%ld", std::lround(r.loc[0] / 10.f), std::lround(r.loc[1] / 10.f),
                  std::lround(r.loc[2] / 10.f));
    return r.key + b;
}

struct Seen { void* actor; Rec rec; };

// Every plant of this world, read, with its identity.
std::vector<Seen> Snapshot(bool host) {
    std::vector<Seen> out;
    std::unordered_map<std::string, int> count;
    for (void* a : LivePlants(host)) {
        Seen s{a, {}};
        if (!Read(a, s.rec)) continue;
        ++count[s.rec.key];
        out.push_back(std::move(s));
    }
    for (Seen& s : out) s.rec.id = Ident(s.rec, count[s.rec.key] > 1);
    return out;
}

void* FindById(const std::string& id, bool host) {
    for (const Seen& s : Snapshot(host))
        if (s.rec.id == id) return s.actor;
    return nullptr;
}

// ---- the wire ------------------------------------------------------------------------------------------

enum Op : uint8_t { kHostState = 1, kHostGone = 2, kClientBorn = 3, kClientState = 4, kClientGone = 5 };
constexpr uint8_t kWireVersion = 2;
constexpr size_t kRecsPerBlob = 96;   // ~150 bytes a record: a blob stays well under the chunk ceiling

void PutU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void PutRaw(std::vector<uint8_t>& b, const void* p, size_t n) { const auto* c = static_cast<const uint8_t*>(p); b.insert(b.end(), c, c + n); }
void PutStr(std::vector<uint8_t>& b, const std::string& s) {
    const size_t n = s.size() > 255 ? 255 : s.size();
    b.push_back(static_cast<uint8_t>(n));
    b.insert(b.end(), s.begin(), s.begin() + static_cast<long>(n));
}

struct Reader {
    const std::vector<uint8_t>& b;
    size_t p = 0;
    bool ok = true;
    uint8_t U8() { if (p + 1 > b.size()) { ok = false; return 0; } return b[p++]; }
    void Raw(void* dst, size_t n) { if (p + n > b.size()) { ok = false; std::memset(dst, 0, n); return; } std::memcpy(dst, b.data() + p, n); p += n; }
    std::string Str() { const size_t n = U8(); if (!ok || p + n > b.size()) { ok = false; return {}; } std::string s(b.begin() + static_cast<long>(p), b.begin() + static_cast<long>(p + n)); p += n; return s; }
};

void PutRec(std::vector<uint8_t>& b, const Rec& r) {
    PutStr(b, r.id); PutStr(b, r.key); PutStr(b, r.cls); PutStr(b, r.plant); PutStr(b, r.attach);
    PutRaw(b, r.loc, sizeof r.loc); PutRaw(b, r.rot, sizeof r.rot); PutRaw(b, r.scale, sizeof r.scale);
    const float f[5] = {r.grow, r.fert, r.water, r.fruit, r.dry};
    PutRaw(b, f, sizeof f);
    PutRaw(b, &r.amount, sizeof r.amount);
}

Rec GetRec(Reader& rd) {
    Rec r;
    r.id = rd.Str(); r.key = rd.Str(); r.cls = rd.Str(); r.plant = rd.Str(); r.attach = rd.Str();
    rd.Raw(r.loc, sizeof r.loc); rd.Raw(r.rot, sizeof r.rot); rd.Raw(r.scale, sizeof r.scale);
    float f[5];
    rd.Raw(f, sizeof f);
    r.grow = f[0]; r.fert = f[1]; r.water = f[2]; r.fruit = f[3]; r.dry = f[4];
    rd.Raw(&r.amount, sizeof r.amount);
    return r;
}

std::vector<std::vector<uint8_t>> EncodeRecs(Op op, const std::vector<Rec>& recs) {
    std::vector<std::vector<uint8_t>> blobs;
    for (size_t i = 0; i < recs.size(); i += kRecsPerBlob) {
        const size_t n = (std::min)(kRecsPerBlob, recs.size() - i);
        std::vector<uint8_t> b;
        PutU8(b, kWireVersion); PutU8(b, op);
        const uint16_t cnt = static_cast<uint16_t>(n);
        PutRaw(b, &cnt, sizeof cnt);
        for (size_t k = 0; k < n; ++k) PutRec(b, recs[i + k]);
        blobs.push_back(std::move(b));
    }
    return blobs;
}

std::vector<uint8_t> EncodeKeys(Op op, const std::vector<std::string>& keys) {
    std::vector<uint8_t> b;
    PutU8(b, kWireVersion); PutU8(b, op);
    const uint16_t cnt = static_cast<uint16_t>((std::min)(keys.size(), size_t{2000}));
    PutRaw(b, &cnt, sizeof cnt);
    for (uint16_t i = 0; i < cnt; ++i) PutStr(b, keys[i]);
    return b;
}

bool SendTo(coop::net::Session* s, int slot, const std::vector<uint8_t>& blob) {
    return coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::PlantSync, Next(g_sequence), blob);
}

// ---- HOST ----------------------------------------------------------------------------------------------

struct Sent { Rec rec; Clock::time_point at; };
std::unordered_map<std::string, Sent> g_sent;            // what the clients were last told of each plant
std::array<bool, coop::net::kMaxPeers> g_owed{};          // a whole garden owed to this slot
Clock::time_point g_nextHostBeat{};
constexpr auto kHostBeat = std::chrono::milliseconds(1000);
constexpr auto kHeartbeat = std::chrono::seconds(15);

bool Differs(const Rec& a, const Rec& b) {
    return a.plant != b.plant || a.attach != b.attach || a.cls != b.cls || a.amount != b.amount ||
           std::fabs(a.grow - b.grow) >= 0.01f || std::fabs(a.water - b.water) >= 0.02f ||
           std::fabs(a.fert - b.fert) >= 0.02f || std::fabs(a.fruit - b.fruit) >= 0.02f ||
           std::fabs(a.dry - b.dry) >= 0.02f || Dist(a.loc, b.loc) > 5.f;
}

void HostBeat(coop::net::Session* s, Clock::time_point now) {
    if (now < g_nextHostBeat) return;
    g_nextHostBeat = now + kHostBeat;
    std::vector<Rec> all, changed;
    std::unordered_set<std::string> live;
    for (Seen& seen : Snapshot(true)) {
        const Rec& r = seen.rec;
        if (!live.insert(r.id).second) continue;   // two plants on one key and one spot: one of them speaks
        all.push_back(r);
        auto it = g_sent.find(r.id);
        if (it == g_sent.end() || Differs(it->second.rec, r) || now - it->second.at >= kHeartbeat) {
            changed.push_back(r);
            g_sent[r.id] = Sent{r, now};
        }
    }
    std::vector<std::string> gone;
    for (auto it = g_sent.begin(); it != g_sent.end();) {
        if (live.count(it->first)) { ++it; continue; }
        gone.push_back(it->first);
        it = g_sent.erase(it);
    }
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (!s->HasPeerConn(slot) || !s->IsSlotWorldReady(slot)) continue;
        const bool whole = g_owed[slot];
        const std::vector<Rec>& recs = whole ? all : changed;
        bool ok = true;
        for (const auto& blob : EncodeRecs(kHostState, recs)) ok = SendTo(s, slot, blob) && ok;
        if (!gone.empty()) ok = SendTo(s, slot, EncodeKeys(kHostGone, gone)) && ok;
        if (whole && ok) {
            g_owed[slot] = false;
            UE_LOGI("plant_sync: the garden (%zu plant(s)) sent to slot %d", all.size(), slot);
        }
        // A refused send of a change is not retried by itself: the heartbeat carries every plant again.
    }
    static int sSaid = 0;
    if ((!changed.empty() || !gone.empty()) && sSaid < 6) {
        ++sSaid;
        UE_LOGI("plant_sync: host beat -- %zu plant(s), %zu changed, %zu gone", all.size(), changed.size(), gone.size());
    }
}

void HostOnClientBlob(coop::net::Session* s, uint8_t slot, Op op, const std::vector<Rec>& recs,
                      const std::vector<std::string>& ids) {
    (void)s;
    if (op == kClientGone) {
        for (const std::string& id : ids) {
            void* a = FindById(id, true);
            if (!a) continue;
            UE_LOGI("plant_sync: slot %u harvested or discarded plant '%s' -- gone here too", slot, id.c_str());
            Destroy(a);
        }
        return;
    }
    for (const Rec& r : recs) {
        void* a = FindById(r.id, true);
        if (op == kClientState) {
            if (!a) continue;
            Apply(a, r);
            g_sent.erase(r.id);   // the next beat tells every peer, the author included
            UE_LOGI("plant_sync: slot %u changed plant '%s' -- water %.2f fert %.2f grow %.2f fruit %.2f amount %d",
                    slot, r.id.c_str(), r.water, r.fert, r.grow, r.fruit, r.amount);
            continue;
        }
        // kClientBorn: a planting on the client. One the host already has (by identity, or the same row on the
        // same spot: both saw the seed fall into the hole) is the host's own, which the client adopts from the
        // next beat; a pot that already holds a live plant refuses a second, as it refuses the host's player.
        if (a) { g_sent.erase(r.id); continue; }
        bool twin = false;
        for (const Seen& h : Snapshot(true))
            if (h.rec.plant == r.plant && Dist(h.rec.loc, r.loc) < 30.f) { twin = true; break; }
        if (twin) {
            UE_LOGI("plant_sync: slot %u's planting of '%s' is a twin of the host's -- the host's stands", slot, r.plant.c_str());
            continue;
        }
        if (!r.attach.empty()) {
            if (void* pot = coop::prop_key_index::Find(Wide(r.attach))) {
                const int32_t off = R::FindPropertyOffset(R::ClassOf(pot), L"plant");
                void* cur = off >= 0 ? Get<void*>(pot, off) : nullptr;
                if (cur && R::IsLive(cur)) {
                    UE_LOGI("plant_sync: slot %u planted into a pot that holds a plant here -- refused", slot);
                    continue;
                }
            }
        }
        void* born = Spawn(r);
        UE_LOGI("plant_sync: slot %u planted '%s' key='%s' -> %s", slot, r.plant.c_str(), r.key.c_str(),
                born ? "planted here" : "SPAWN FAILED");
        g_sent.erase(r.id);
    }
}

// ---- CLIENT --------------------------------------------------------------------------------------------

struct Known { void* actor = nullptr; int32_t idx = -1; Rec host; bool haveHost = false; };
std::unordered_map<std::string, Known> g_known;   // by identity: plants the host has (told, or loaded with its save)
struct Unconfirmed { int32_t idx = -1; Clock::time_point seen{}; bool reported = false; Clock::time_point reportedAt{}; };
std::unordered_map<void*, Unconfirmed> g_unconfirmed;   // plants born here the host has not named
std::unordered_set<std::string> g_expectGone;            // destroyed here on the host's word
std::unordered_map<std::string, Clock::time_point> g_lastIntent;
std::unordered_map<void*, Clock::time_point> g_touched;  // a plant the player acted on (harvest, discard)
bool g_baselined = false;
Clock::time_point g_plantVerbUntil{}, g_careVerbUntil{}, g_nextClientBeat{};
constexpr auto kClientBeat = std::chrono::milliseconds(500);
constexpr auto kWildGrace = std::chrono::seconds(3);       // a plant born here is waited on this long before it is called wild
constexpr auto kReportGrace = std::chrono::seconds(8);     // a reported planting the host never named is taken back

void Track(const std::string& id, void* a, const Rec& host, bool haveHost) {
    Known& k = g_known[id];
    k.actor = a;
    k.idx = R::InternalIndexOf(a);
    if (haveHost) { k.host = host; k.haveHost = true; }
    else if (!k.haveHost) k.host = host;
}

void* KnownActor(const std::string& id) {
    auto it = g_known.find(id);
    return (it != g_known.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) ? it->second.actor : nullptr;
}

void ClientOnHostBlob(Op op, const std::vector<Rec>& recs, const std::vector<std::string>& ids) {
    if (op == kHostGone) {
        for (const std::string& id : ids) {
            void* a = KnownActor(id);
            if (!a) a = FindById(id, false);
            g_known.erase(id);
            if (!a) continue;
            g_expectGone.insert(id);
            g_unconfirmed.erase(a);
            Destroy(a);
        }
        return;
    }
    const std::vector<Seen> here = Snapshot(false);
    for (const Rec& r : recs) {
        void* a = KnownActor(r.id);
        if (!a)
            for (const Seen& h : here)
                if (h.rec.id == r.id && R::IsLive(h.actor)) { a = h.actor; break; }
        if (!a) {
            // The same row on the same spot, born here and not yet named: this peer's own planting of the
            // host's plant. It takes the host's key instead of standing beside a second one.
            for (auto& [u, info] : g_unconfirmed) {
                if (!R::IsLiveByIndex(u, info.idx)) continue;
                Rec mine;
                if (!Read(u, mine) || mine.plant != r.plant || Dist(mine.loc, r.loc) >= 30.f) continue;
                SetNameAt(u, OffsFor(R::ClassOf(u)).key, r.key);
                a = u;
                UE_LOGI("plant_sync: this peer's '%s' at the host's spot takes the host's key='%s'", r.plant.c_str(), r.key.c_str());
                break;
            }
        }
        if (a) {
            Apply(a, r);
            LinkPot(a, r.attach);
        } else {
            a = Spawn(r);
            if (!a) continue;
            static int sSaid = 0;
            if (sSaid++ < 8) UE_LOGI("plant_sync: the host's '%s' '%s' planted here", r.plant.c_str(), r.id.c_str());
        }
        g_unconfirmed.erase(a);
        Track(r.id, a, r, true);
    }
}

void SendIntent(coop::net::Session* s, Op op, const Rec* r, const std::string& id) {
    std::vector<uint8_t> blob;
    if (r) {
        auto blobs = EncodeRecs(op, {*r});
        if (blobs.empty()) return;
        blob = std::move(blobs.front());
    } else {
        blob = EncodeKeys(op, {id});
    }
    if (!SendTo(s, 0, blob))
        UE_LOGW("plant_sync: an intent for plant '%s' was refused by the link (it is not retried)", id.c_str());
}

void ClientBeat(coop::net::Session* s, Clock::time_point now) {
    if (now < g_nextClientBeat) return;
    g_nextClientBeat = now + kClientBeat;
    if (!coop::join_membership_sweep::HasLoadTailQuiesced()) return;
    const std::vector<Seen> plants = Snapshot(false);
    if (!g_baselined) {
        // The world this client loaded is the host's save: every plant in it is the host's too.
        for (const Seen& p : plants)
            if (!g_known.count(p.rec.id)) Track(p.rec.id, p.actor, p.rec, false);
        g_baselined = true;
        UE_LOGI("plant_sync: %zu plant(s) in the loaded world, the host's", g_known.size());
        return;
    }
    // Gone here: by the host's word, or by this player's hand (a harvest, a discard, a felled tree).
    for (auto it = g_known.begin(); it != g_known.end();) {
        if (R::IsLiveByIndex(it->second.actor, it->second.idx)) { ++it; continue; }
        if (!g_expectGone.erase(it->first)) {
            UE_LOGI("plant_sync: plant '%s' is gone here -- the host is told", it->first.c_str());
            SendIntent(s, kClientGone, nullptr, it->first);
        }
        it = g_known.erase(it);
    }
    for (auto it = g_touched.begin(); it != g_touched.end();) it = now >= it->second ? g_touched.erase(it) : std::next(it);
    const bool caring = now < g_careVerbUntil;
    ue_wrap::FVector me{};
    void* local = coop::players::Registry::Get().Local();
    const bool haveMe = local && E::TryGetActorLocation(local, me);
    for (const Seen& p : plants) {
        void* a = p.actor;
        const Rec& r = p.rec;
        auto k = g_known.find(r.id);
        if (k != g_known.end()) {
            k->second.actor = a;
            k->second.idx = R::InternalIndexOf(a);
            // A change by this player's verb: the care tools near the player, a harvest on the plant itself.
            const float pos[3] = {me.X, me.Y, me.Z};
            const bool near = haveMe && Dist(r.loc, pos) < 600.f;
            if (!(caring && near) && !g_touched.count(a)) continue;
            const Rec& h = k->second.host;
            const bool cared = r.water > h.water + 0.05f || r.fert > h.fert + 0.05f || r.grow > h.grow + 0.05f;
            const bool picked = r.amount < h.amount || r.fruit < h.fruit - 0.1f;
            if (!cared && !picked) continue;
            auto li = g_lastIntent.find(r.id);
            if (li != g_lastIntent.end() && now - li->second < std::chrono::milliseconds(1500)) continue;
            g_lastIntent[r.id] = now;
            SendIntent(s, kClientState, &r, r.id);
            k->second.host = r;   // the host's echo confirms it; until then this is what it was told
            UE_LOGI("plant_sync: this player %s plant '%s' -- water %.2f fert %.2f grow %.2f amount %d (intent)",
                    picked ? "picked" : "tended", r.id.c_str(), r.water, r.fert, r.grow, r.amount);
            continue;
        }
        auto u = g_unconfirmed.find(a);
        if (u == g_unconfirmed.end()) {
            g_unconfirmed[a] = Unconfirmed{R::InternalIndexOf(a), now, false, {}};
            continue;
        }
        Unconfirmed& info = u->second;
        if (!info.reported && info.seen <= g_plantVerbUntil) {
            info.reported = true;
            info.reportedAt = now;
            SendIntent(s, kClientBorn, &r, r.id);
            UE_LOGI("plant_sync: this player planted '%s' key='%s' -- the host is asked", r.plant.c_str(), r.key.c_str());
            continue;
        }
        const bool wild = !info.reported && now - info.seen > kWildGrace;
        const bool refused = info.reported && now - info.reportedAt > kReportGrace;
        if (wild || refused) {
            UE_LOGI("plant_sync: '%s' key='%s' born here %s -- removed (the host's garden is the one that grows)",
                    r.plant.c_str(), r.key.c_str(), wild ? "on its own (a wild spawner)" : "and never named by the host");
            g_unconfirmed.erase(u);
            Destroy(a);
        }
    }
    for (auto it = g_unconfirmed.begin(); it != g_unconfirmed.end();)
        it = R::IsLiveByIndex(it->first, it->second.idx) ? std::next(it) : g_unconfirmed.erase(it);
}

// ---- the client's verbs and refusals -------------------------------------------------------------------

UE_FACT_FN(kRowPotUse,     "plant_sync", L"prop_farmPot_C",    L"playerUsedOn",       Declared);
UE_FACT_FN(kRowBury,       "plant_sync", L"dirthole_C",        L"processBuriedItem",  Declared);
UE_FACT_FN(kRowPlantAct,   "plant_sync", L"growingPlant_C",    L"actionOptionIndex",  Declared);
UE_FACT_FN(kRowWaterCan,   "plant_sync", L"prop_waterCan_C",   L"playerHandUse_LMB",  Declared);
UE_FACT_FN(kRowFertilizer, "plant_sync", L"prop_fertilizer_C", L"playerHandUse_LMB",  Declared);
UE_FACT_FN(kRowGrowinator, "plant_sync", L"prop_growinator_C", L"fired",              Declared);
UE_FACT_FN(kRowAddFruit,   "plant_sync", L"growingPlant_C",    L"addFruit",           Declared);
UE_FACT_FN(kRowTurnDead,   "plant_sync", L"growingPlant_C",    L"turnDead",           Declared);
constexpr int kTag = 0x504C4E54;  // 'PLNT'

bool ClientLive() {
    auto* s = g_session.load(std::memory_order_acquire);
    return Active(s) && !IsHost(s);
}

void OnPlantVerbPost(const sg::Call& c) {
    if (c.fromOurCode || !ClientLive()) return;
    g_plantVerbUntil = Clock::now() + std::chrono::seconds(3);
}
void OnCareVerbPost(const sg::Call& c) {
    if (c.fromOurCode || !ClientLive()) return;
    g_careVerbUntil = Clock::now() + std::chrono::seconds(2);
}
void OnPlantActPost(const sg::Call& c) {
    if (c.fromOurCode || !ClientLive() || !c.object) return;
    g_touched[c.object] = Clock::now() + std::chrono::seconds(2);
}
// A client's copy hangs no fruit of its own (the host's fruit props arrive by the prop lanes) and does not die on
// its own clock (the host's dead row arrives on the record).
sg::Verdict OnHostOwnedPre(const sg::Call& c) {
    if (c.fromOurCode || !ClientLive() || !c.object) return sg::Verdict::Run;
    if (!coop::net_pump::IsInAnnouncedWorld(c.object)) return sg::Verdict::Run;
    return sg::Verdict::Cancel;
}

struct Armed { const ue_wrap::facts::Function* row; sg::PreFn pre; sg::PostFn post; bool on; };
Armed g_armed[] = {
    {&kRowPotUse, nullptr, &OnPlantVerbPost, false},  {&kRowBury, nullptr, &OnPlantVerbPost, false},
    {&kRowPlantAct, nullptr, &OnPlantActPost, false}, {&kRowWaterCan, nullptr, &OnCareVerbPost, false},
    {&kRowFertilizer, nullptr, &OnCareVerbPost, false}, {&kRowGrowinator, nullptr, &OnCareVerbPost, false},
    {&kRowAddFruit, &OnHostOwnedPre, nullptr, false}, {&kRowTurnDead, &OnHostOwnedPre, nullptr, false},
};

void ArmWatches() {
    static Clock::time_point sNext{};
    const auto now = Clock::now();
    if (now < sNext) return;
    sNext = now + std::chrono::seconds(1);
    for (Armed& w : g_armed) {
        if (w.on) continue;
        w.on = sg::WatchName(*w.row, kTag, w.pre, w.post);
        if (w.on) UE_LOGI("plant_sync: watching %ls::%ls", w.row->Class(), w.row->Name());
    }
}

// The world changed under the lane: every key it holds named the old one.
void Clear() {
    g_asm.Clear();
    g_sent.clear();
    g_owed.fill(false);
    g_known.clear();
    g_unconfirmed.clear();
    g_expectGone.clear();
    g_lastIntent.clear();
    g_touched.clear();
    g_baselined = false;
    g_world = ue_wrap::world_identity::Generation();
}

void World(coop::net::Session* s) {
    if (g_world == ue_wrap::world_identity::Generation()) return;
    Clear();
    // A host's new world is owed whole to every client once it stands in it.
    if (IsHost(s))
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) g_owed[slot] = s->HasPeerConn(slot);
}

}  // namespace

void Install(coop::net::Session* session) {
    if (g_session.load(std::memory_order_acquire) != session) {
        g_session.store(session, std::memory_order_release);
        Clear();
    }
}

void Tick() {
    if (!ue_wrap::game_thread::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!Active(s)) return;
    ArmWatches();
    World(s);
    const auto now = Clock::now();
    if (!g_asm.Idle()) {
        static Clock::time_point sSweep{};
        if (now >= sSweep) { sSweep = now + std::chrono::seconds(1); g_asm.Sweep(now, std::chrono::seconds(10)); }
    }
    if (IsHost(s)) HostBeat(s, now);
    else ClientBeat(s, now);
}

void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot) {
    if (!ue_wrap::game_thread::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!Active(s) || senderSlot >= coop::net::kMaxPeers) return;
    World(s);
    const bool host = IsHost(s);
    if (host ? (senderSlot == 0 || !s->IsSlotWorldReady(senderSlot)) : senderSlot != 0) return;
    if (chunk.chunks > 64) return;
    std::vector<uint8_t> bytes;
    if (!g_asm.OnChunk(chunk, senderSlot, bytes)) return;
    Reader rd{bytes};
    const uint8_t ver = rd.U8();
    const Op op = static_cast<Op>(rd.U8());
    uint16_t cnt = 0;
    rd.Raw(&cnt, sizeof cnt);
    if (!rd.ok || ver != kWireVersion) {
        UE_LOGW("plant_sync: a blob of wire version %u from slot %u -- dropped", ver, senderSlot);
        return;
    }
    std::vector<Rec> recs;
    std::vector<std::string> keys;
    const bool keyOp = op == kHostGone || op == kClientGone;
    for (uint16_t i = 0; i < cnt && rd.ok; ++i) {
        if (keyOp) keys.push_back(rd.Str());
        else recs.push_back(GetRec(rd));
    }
    if (!rd.ok) {
        UE_LOGW("plant_sync: a malformed blob (op %u) from slot %u -- dropped", static_cast<unsigned>(op), senderSlot);
        return;
    }
    const bool fromHostOp = op == kHostState || op == kHostGone;
    if (host == fromHostOp) return;   // a host takes only intents, a client only the host's garden
    if (host) HostOnClientBlob(s, senderSlot, op, recs, keys);
    else ClientOnHostBlob(op, recs, keys);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (peerSlot > 0 && peerSlot < static_cast<int>(coop::net::kMaxPeers)) g_owed[peerSlot] = true;
}

void OnPeerGone(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_owed[peerSlot] = false;
    g_asm.ClearSlot(static_cast<uint8_t>(peerSlot));
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    Clear();
    g_sequence = 0;
}

}  // namespace coop::plant_sync
