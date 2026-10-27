// coop/creatures/walker_mirror.cpp -- see coop/creatures/walker_mirror.h.

#include "coop/creatures/walker_mirror.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_wire_parity.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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
sg::Verdict OnBrainPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
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
    g_nextMs = 0;
}

}  // namespace coop::walker_mirror
