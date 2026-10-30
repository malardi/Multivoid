// coop/world/pipe_burst_sync.cpp -- see coop/world/pipe_burst_sync.h.

#include "coop/world/pipe_burst_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::pipe_burst_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowTryBurst, "pipe_burst_sync", L"basementDrain_C", L"tryBurstPipe", Declared);
constexpr int kTagTryBurst = 0x50495042;  // 'PIPB'

constexpr uint64_t kPollMs = 1000;
constexpr uint64_t kFullMs = 5000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false, g_refused = false;

void* g_pipeCls = nullptr;     // burstWaterPipe_C
int32_t g_damagedOff = -2;     // burstWaterPipe_C.damaged
void* g_stateFn = nullptr;     // burstWaterPipe_C.state(damaged)

std::unordered_map<std::string, uint8_t> g_lastSent;  // host: the state each pipe was last sent with
uint64_t g_lastPollMs = 0, g_lastFullMs = 0;
uint64_t g_rollsRefused = 0, g_sent = 0, g_applied = 0, g_unresolved = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Resolve() {
    if (!g_pipeCls) g_pipeCls = OI::ClassByName(L"burstWaterPipe_C");
    if (!g_pipeCls) return false;
    if (g_damagedOff == -2) g_damagedOff = R::FindPropertyOffset(g_pipeCls, L"damaged");
    if (!g_stateFn) g_stateFn = R::FindDispatchFunctionCached(g_pipeCls, L"state");
    return g_damagedOff >= 0;
}

// A pipe is named on the wire by where it stands ("@x,y,z", whole units), not by its object name: a world loaded
// back from the save (a return from a dimension, a reload) names the save's pipes burstWaterPipe_C_<n>, which no
// other peer shares, and the host's every pipe state then went nowhere ("the host's pipe ... is not in this world").
std::string PosKey(void* pipe) {
    ue_wrap::FVector a{};
    if (!E::TryGetActorLocation(pipe, a)) return std::string();
    char b[64];
    std::snprintf(b, sizeof(b), "@%d,%d,%d", static_cast<int>(std::lround(a.X)), static_cast<int>(std::lround(a.Y)),
                  static_cast<int>(std::lround(a.Z)));
    return b;
}

std::string NarrowName(void* obj) {
    const std::wstring w = R::ToString(R::NameOf(obj));
    std::string n;
    n.reserve(w.size());
    for (wchar_t c : w) n.push_back(static_cast<char>(c & 0x7f));
    return n;
}

uint8_t ReadDamaged(void* pipe) {
    return reinterpret_cast<const uint8_t*>(pipe)[g_damagedOff] ? 1 : 0;
}

sg::Verdict OnTryBurstPre(const sg::Call&) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (++g_rollsRefused <= 3) UE_LOGI("[PIPE] the drain's burst roll is the host's -- refused here");
    return sg::Verdict::Cancel;
}

struct Collect { std::vector<std::pair<std::string, uint8_t>>* out; };

void HostPoll(coop::net::Session& s, bool full) {
    if (!Resolve()) return;
    std::vector<std::pair<std::string, uint8_t>> pipes;
    Collect c{&pipes};
    OI::ForEachInstance(g_pipeCls, [](void* ctx, void* obj, int32_t) {
        auto* cc = static_cast<Collect*>(ctx);
        if (NarrowName(obj).rfind("Default__", 0) == 0) return;
        // The pipes of the world just left linger until the collector runs (renamed burstWaterPipe_C_<n>): only
        // the current world's are this world's state.
        void* cur = ue_wrap::world_identity::CurrentWorld();
        void* its = ue_wrap::world_identity::WorldOf(obj);
        if (cur && its && its != cur) return;
        std::string n = PosKey(obj);
        if (n.empty()) return;
        cc->out->emplace_back(std::move(n), ReadDamaged(obj));
    }, &c);
    for (const auto& [name, damaged] : pipes) {
        if (name.size() >= sizeof(coop::net::PipeStatePayload::name)) continue;
        auto it = g_lastSent.find(name);
        const bool changed = it == g_lastSent.end() || it->second != damaged;
        if (!changed && !full) continue;
        coop::net::PipeStatePayload p{};
        std::memcpy(p.name, name.data(), name.size());
        p.damaged = damaged;
        if (!s.SendReliable(coop::net::ReliableKind::PipeState, &p, sizeof(p))) continue;
        if (changed && it != g_lastSent.end())
            UE_LOGI("[PIPE] host pipe '%s' %s -- sent", name.c_str(), damaged ? "burst" : "fixed");
        g_lastSent[name] = damaged;
        ++g_sent;
    }
}

struct FindByName { const char* name; void* found; };
struct FindNear { float x, y, z; void* found; float bestD2; };

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched || g_refused) return;
    if (sg::WatchName(kRowTryBurst, kTagTryBurst, &OnTryBurstPre, nullptr)) { g_watched = true; return; }
    g_refused = true;
    UE_LOGE("[PIPE] the script-body gate refused the drain's burst watch -- each peer bursts its own pipes");
}

void Tick() {
    if (!g_watched && !g_refused) Install(g_session.load(std::memory_order_acquire));
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || s->role() != coop::net::Role::Host) return;
    const uint64_t now = NowMs();
    if (now - g_lastPollMs < kPollMs) return;
    g_lastPollMs = now;
    const bool full = now - g_lastFullMs >= kFullMs;
    if (full) g_lastFullMs = now;
    HostPoll(*s, full);
}

void OnPipeState(const coop::net::PipeStatePayload& p) {
    if (!Resolve()) return;
    FindByName f{p.name, nullptr};
    float px = 0, py = 0, pz = 0;
    if (p.name[0] == '@' && std::sscanf(p.name + 1, "%f,%f,%f", &px, &py, &pz) == 3) {
        // By place: this world's pipe within a metre of the host's.
        FindNear n{px, py, pz, nullptr, 100.f * 100.f};
        OI::ForEachInstance(g_pipeCls, [](void* ctx, void* obj, int32_t) {
            auto* nn = static_cast<FindNear*>(ctx);
            void* cur = ue_wrap::world_identity::CurrentWorld();
            void* its = ue_wrap::world_identity::WorldOf(obj);
            if (cur && its && its != cur) return;
            ue_wrap::FVector a{};
            if (!E::TryGetActorLocation(obj, a)) return;
            const float dx = a.X - nn->x, dy = a.Y - nn->y, dz = a.Z - nn->z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < nn->bestD2) { nn->bestD2 = d2; nn->found = obj; }
        }, &n);
        f.found = n.found;
    } else {
        OI::ForEachInstance(g_pipeCls, [](void* ctx, void* obj, int32_t) {
            auto* ff = static_cast<FindByName*>(ctx);
            if (!ff->found && NarrowName(obj) == ff->name) ff->found = obj;
        }, &f);
    }
    if (!f.found) {
        if (++g_unresolved <= 5) UE_LOGW("[PIPE] the host's pipe '%s' is not in this world", p.name);
        return;
    }
    if (ReadDamaged(f.found) == p.damaged) return;
    bool ran = false;
    if (g_stateFn) {
        ue_wrap::ParamFrame fr(g_stateFn);
        fr.Set<bool>(L"damaged", p.damaged != 0);
        ran = ue_wrap::Call(f.found, fr);
    }
    if (!ran) reinterpret_cast<uint8_t*>(f.found)[g_damagedOff] = p.damaged;
    ++g_applied;
    UE_LOGI("[PIPE] pipe '%s' %s, as the host has it%s", p.name, p.damaged ? "burst" : "fixed",
            ran ? "" : " (state() did not run; the flag alone)");
}

void OnDisconnect() {
    if (g_sent || g_applied || g_rollsRefused || g_unresolved)
        UE_LOGI("[PIPE] session end -- states sent=%llu applied=%llu rolls refused=%llu unresolved=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_rollsRefused), static_cast<unsigned long long>(g_unresolved));
    g_lastSent.clear();
    g_lastPollMs = g_lastFullMs = 0;
    g_sent = g_applied = g_rollsRefused = g_unresolved = 0;
}

}  // namespace coop::pipe_burst_sync
