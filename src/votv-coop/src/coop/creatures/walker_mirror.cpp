// coop/creatures/walker_mirror.cpp -- see coop/creatures/walker_mirror.h.

#include "coop/creatures/walker_mirror.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_wire_parity.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/script_fingerprint.h"  // the walker brain dump (devkeys)
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"  // FindNavPath: the route to the nearest player
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::walker_mirror {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
namespace pf = coop::net::propspawn_flags;

// The walker's brain, entry by entry: its tick (the chase, the look test, the path follow), its five
// timers (the jump, the unhook, the weapon pickup, the saw, the damage) and the nav path update. Its
// BeginPlay stays, so the copy is built and animated as the game builds it.
UE_FACT_FN(kRowTick,      "walker_mirror", L"prop_wMannequin_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowJump,      "walker_mirror", L"prop_wMannequin_C", L"timer_jump", Declared);
UE_FACT_FN(kRowUnhook,    "walker_mirror", L"prop_wMannequin_C", L"timer_unhook", Declared);
UE_FACT_FN(kRowPickup,    "walker_mirror", L"prop_wMannequin_C", L"timer_pickupWeapon", Declared);
UE_FACT_FN(kRowSaw,       "walker_mirror", L"prop_wMannequin_C", L"timer_saw", Declared);
UE_FACT_FN(kRowDamage,    "walker_mirror", L"prop_wMannequin_C", L"timer_damage", Declared);
UE_FACT_FN(kRowUpd,       "walker_mirror", L"prop_wMannequin_C", L"upd", Declared);

struct Row { const ue_wrap::facts::Function* row; int tag; const char* what; };
const Row kRows[] = {
    {&kRowTick,   0x574D5431, "ReceiveTick"},
    {&kRowJump,   0x574D5432, "timer_jump"},
    {&kRowUnhook, 0x574D5433, "timer_unhook"},
    {&kRowPickup, 0x574D5434, "timer_pickupWeapon"},
    {&kRowSaw,    0x574D5435, "timer_saw"},
    {&kRowDamage, 0x574D5436, "timer_damage"},
    {&kRowUpd,    0x574D5437, "upd"},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg g_reg[kRowCount] = {};

constexpr uint64_t kPeriodMs  = 100;          // the host's walker reaches the clients at 10 Hz
constexpr float    kResendCm2 = 2.0f * 2.0f;  // and only when it moved more than 2 cm

std::atomic<coop::net::Session*> g_session{nullptr};
void* g_cls = nullptr;
uint64_t g_nextMs = 0;
uint64_t g_refused = 0, g_sent = 0;
bool g_saidLive = false;

struct Sent { ue_wrap::FVector pos; float yaw; };
std::unordered_map<uint32_t, Sent> g_lastSent;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A client's walker is the host's walker shown here: its own brain does not run.
void HostBrainPre(void* w);  // below

sg::Verdict OnBrainPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) {
        if (call.tag == kRows[0].tag && call.object && s->AnyWorldReadyPeer()) HostBrainPre(call.object);
        return sg::Verdict::Run;
    }
    if (!call.object || !coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    ++g_refused;
    return sg::Verdict::Cancel;
}

void Collect(void* ctx, void* obj, int32_t /*index*/) { static_cast<std::vector<void*>*>(ctx)->push_back(obj); }

void SendWalker(coop::net::Session& s, void* w) {
    const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(w);
    if (eid == coop::element::kInvalidId || eid == 0) return;
    ue_wrap::FVector cur{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(w, cur) || !E::TryGetActorRotation(w, rot)) return;
    const uint32_t key = static_cast<uint32_t>(eid);
    const auto it = g_lastSent.find(key);
    if (it != g_lastSent.end()) {
        const float dx = cur.X - it->second.pos.X, dy = cur.Y - it->second.pos.Y, dz = cur.Z - it->second.pos.Z;
        float dyaw = rot.Yaw - it->second.yaw;
        if (dyaw < 0) dyaw = -dyaw;
        if (dx * dx + dy * dy + dz * dz <= kResendCm2 && dyaw < 2.f) return;
    }
    coop::net::PropSnapPosPayload p{};
    p.eid = key;
    p.locX = cur.X; p.locY = cur.Y; p.locZ = cur.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    p.physFlags = coop::prop_wire_parity::PhysFlagsOf(w);
    const coop::prop_drive_host::DriveContext ctx = coop::prop_drive_host::ContextOf(w, key);
    if (ctx.open) return;  // a client carries it: its stream moves the walker
    p.driveGen  = ctx.gen;
    p.driveOpen = 0;
    if (!s.SendReliable(coop::net::ReliableKind::PropSnapPos, &p, sizeof(p))) return;
    g_lastSent[key] = Sent{cur, rot.Yaw};
    if (++g_sent == 1) UE_LOGI("[WALKER] HOST streams its walking mannequin eid=%u to the clients", key);
}

// [devkeys] The walker's Blueprint, once a process, as the canonical text the fingerprint renders: the
// chase target and the look test are read from it to make the walker chase the nearest player. Every line
// goes to the log under [WALKER-BP]. Only with the developer switch on; inert otherwise.
bool g_dumped = false;
void DumpBrainOnce() {
    if (g_dumped || !g_cls) return;
    static const bool s_on = coop::config::ResolveFlag(coop::config_registry::rows::devkeys);
    if (!s_on) { g_dumped = true; return; }
    g_dumped = true;
    const wchar_t* const kFns[] = {L"ExecuteUbergraph_prop_wMannequin", L"ReceiveTick", L"upd", L"pickUpWeapon",
                                   L"dropWeapon", L"setWeapon", L"timer_jump", L"timer_saw", L"timer_damage"};
    for (const wchar_t* name : kFns) {
        void* fn = R::FindFunction(g_cls, name);
        if (!fn) { UE_LOGI("[WALKER-BP] %ls: not found", name); continue; }
        const auto r = ue_wrap::script_fingerprint::Of(fn);
        UE_LOGI("[WALKER-BP] ==== %ls ok=%d lines follow%s%s", name, r.ok ? 1 : 0, r.problem.empty() ? "" : " problem=",
                r.problem.c_str());
        std::istringstream in(r.text);
        std::string line;
        int n = 0;
        while (std::getline(in, line) && n < 4000) UE_LOGI("[WALKER-BP] %04d %s", n++, line.c_str());
    }
    UE_LOGI("[WALKER-BP] ==== end");
}

// ---- the host's walker sees every player and chases the nearest -------------------------------------------
// Read from the walker's Blueprint ([WALKER-BP]): its tick sets `looking` = ((the box in player 0's frustum and
// rendered) or `saw`) and not `angry`, and stands still while it holds; otherwise it steps along `Path` from
// `ind`, a route its graph asks every half second toward player 0's camera and copies in at `upd` and after its
// move request. Player 0 is the host, so a client was never seen and never chased. On the host: a client whose
// aim holds the walker in a 50-degree cone within 40 m sets `saw` (the game's own "seen" latch) before the tick
// reads it, and when the nearest player is a client the walker's `Path` is the nav route to that client,
// written again whenever the graph copies its own route in.
struct BrainOffsets { int32_t path = -2, ind = -2, saw = -2; uint8_t sawMask = 0; };
BrainOffsets g_off;

struct VecArray { ue_wrap::FVector* data; int32_t num; int32_t max; };

struct Chase {
    void* walker = nullptr;
    std::vector<ue_wrap::FVector> path;  // our route to the client, empty when the host is the nearest
    uint64_t nextRouteMs = 0;
    bool forcedSaw = false;
};
std::unordered_map<void*, Chase> g_chase;

bool ResolveBrain() {
    if (g_off.path != -2) return g_off.path >= 0 && g_off.ind >= 0 && g_off.saw >= 0;
    g_off.path = R::FindPropertyOffset(g_cls, L"Path");
    g_off.ind  = R::FindPropertyOffset(g_cls, L"ind");
    int32_t off = -1;
    uint8_t mask = 0;
    if (R::FindBoolProperty(g_cls, L"saw", off, mask)) { g_off.saw = off; g_off.sawMask = mask; } else g_off.saw = -1;
    const bool ok = g_off.path >= 0 && g_off.ind >= 0 && g_off.saw >= 0;
    UE_LOGI("[WALKER] host brain fields: Path@%d ind@%d saw@%d -- %s", g_off.path, g_off.ind, g_off.saw,
            ok ? "the walker sees and chases every player" : "unresolved, it keeps the game's host-only brain");
    return ok;
}

void WriteSaw(void* w, bool on) {
    uint8_t* b = reinterpret_cast<uint8_t*>(w) + g_off.saw;
    *b = on ? static_cast<uint8_t>(*b | g_off.sawMask) : static_cast<uint8_t>(*b & ~g_off.sawMask);
}

// The walker's Path as `pts`: in place when it fits, else a new engine buffer (the old one freed). ind to 0.
void WritePath(void* w, const std::vector<ue_wrap::FVector>& pts) {
    auto* arr = reinterpret_cast<VecArray*>(reinterpret_cast<uint8_t*>(w) + g_off.path);
    const int32_t n = static_cast<int32_t>(pts.size());
    if (n <= 0) return;
    if (!arr->data || arr->max < n) {
        auto* buf = static_cast<ue_wrap::FVector*>(R::EngineAlloc(sizeof(ue_wrap::FVector) * static_cast<size_t>(n)));
        if (!buf) return;
        if (arr->data) R::EngineFree(arr->data);
        arr->data = buf;
        arr->max = n;
    }
    for (int32_t i = 0; i < n; ++i) arr->data[i] = pts[static_cast<size_t>(i)];
    arr->num = n;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(w) + g_off.ind) = 0;
}

bool PathIs(void* w, const std::vector<ue_wrap::FVector>& pts) {
    const auto* arr = reinterpret_cast<const VecArray*>(reinterpret_cast<const uint8_t*>(w) + g_off.path);
    if (!arr->data || arr->num != static_cast<int32_t>(pts.size()) || pts.empty()) return false;
    const ue_wrap::FVector& a = arr->data[arr->num - 1];
    const ue_wrap::FVector& b = pts.back();
    return std::fabs(a.X - b.X) < 1.f && std::fabs(a.Y - b.Y) < 1.f && std::fabs(a.Z - b.Z) < 1.f;
}

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

// Whether any client's aim holds the walker: 50 degrees either side of where it looks, within 40 m.
bool ClientSees(const ue_wrap::FVector& at) {
    constexpr float kCos = 0.64f;         // cos(50 deg)
    constexpr float kRange2 = 4000.f * 4000.f;
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = reg.Puppet(slot);
        if (!rp || !rp->valid() || !rp->GetActor()) continue;
        ue_wrap::FVector eye{};
        if (!E::TryGetActorLocation(rp->GetActor(), eye)) continue;
        eye.Z += 64.f;
        const float d2 = Dist2(at, eye);
        if (d2 > kRange2 || d2 < 1.f) continue;
        const float inv = 1.f / std::sqrt(d2);
        const ue_wrap::FVector dir{(at.X - eye.X) * inv, (at.Y - eye.Y) * inv, (at.Z - eye.Z) * inv};
        const ue_wrap::FVector aim = rp->GetSyncedAimDirection();
        if (aim.X * dir.X + aim.Y * dir.Y + aim.Z * dir.Z >= kCos) return true;
    }
    return false;
}

// The nearest player's place: the host's own (false: the game's brain already chases it) or a client's.
bool NearestClient(const ue_wrap::FVector& at, ue_wrap::FVector& out) {
    auto& reg = coop::players::Registry::Get();
    float best = 1e30f;
    bool client = false;
    if (void* local = reg.Local()) {
        ue_wrap::FVector l{};
        if (E::TryGetActorLocation(local, l)) best = Dist2(at, l);
    }
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = reg.Puppet(slot);
        if (!rp || !rp->valid() || !rp->GetActor()) continue;
        ue_wrap::FVector l{};
        if (!E::TryGetActorLocation(rp->GetActor(), l)) continue;
        const float d2 = Dist2(at, l);
        if (d2 < best) { best = d2; out = l; client = true; }
    }
    return client && best <= 20000.f * 20000.f;  // the game's own chase range
}

// Before the host walker's tick: a client's look latches `saw`; the route to a nearer client is in place.
void HostBrainPre(void* w) {
    if (!ResolveBrain()) return;
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(w, at)) return;
    Chase& c = g_chase[w];
    c.walker = w;
    const bool sees = ClientSees(at);
    if (sees) { WriteSaw(w, true); c.forcedSaw = true; }
    else if (c.forcedSaw) { WriteSaw(w, false); c.forcedSaw = false; }
    const uint64_t now = NowMs();
    if (now >= c.nextRouteMs) {
        c.nextRouteMs = now + 500;
        ue_wrap::FVector target{};
        c.path.clear();
        if (NearestClient(at, target)) E::FindNavPath(w, at, target, c.path);
    }
    if (!c.path.empty() && !PathIs(w, c.path)) WritePath(w, c.path);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    for (int i = 0; i < kRowCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kRows[i].row, kRows[i].tag, &OnBrainPre, nullptr)) { g_reg[i] = Reg::Registered; continue; }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[WALKER] the script-body gate refused the watch on '%s' -- a client's walker keeps that part of its brain",
                kRows[i].what);
    }
}

void Tick(coop::net::Session& session) {
    if (!g_saidLive) {
        int live = 0;
        for (int i = 0; i < kRowCount; ++i)
            if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kRows[i].row, kRows[i].tag)) ++live;
        if (live == kRowCount) { g_saidLive = true; UE_LOGI("[WALKER] the client walker brain gates are live"); }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    const uint64_t now = NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kPeriodMs;
    if (!session.AnyWorldReadyPeer()) return;
    if (!g_cls) g_cls = OI::ClassByName(L"prop_wMannequin_C");
    if (!g_cls) return;
    DumpBrainOnce();
    std::vector<void*> walkers;
    OI::ForEachInstance(g_cls, &Collect, &walkers);
    for (void* w : walkers) SendWalker(session, w);
}

void OnDisconnect() {
    if (g_refused || g_sent)
        UE_LOGI("[WALKER] session end -- client brain calls refused=%llu, host poses sent=%llu",
                static_cast<unsigned long long>(g_refused), static_cast<unsigned long long>(g_sent));
    g_refused = g_sent = 0;
    g_lastSent.clear();
    g_chase.clear();
    g_nextMs = 0;
}

}  // namespace coop::walker_mirror
