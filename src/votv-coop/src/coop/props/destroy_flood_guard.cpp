// coop/props/destroy_flood_guard.cpp -- see coop/props/destroy_flood_guard.h.

#include "coop/props/destroy_flood_guard.h"

#include "coop/element/registry.h"      // LivePropActor: a refused eid-only destroy's prop
#include "coop/net/net_clock.h"
#include "coop/props/prop_key_index.h"  // Find: a refused keyed destroy's prop
#include "coop/props/prop_snapshot.h"   // ExpressIncrementalSpawn: the host's copy given back
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>

namespace coop::destroy_flood_guard {

namespace {

// The budget. A player destroys props one verb at a time: eating, burning, breaking, a sale, a trash
// pile grabbed; an explosion or a crate opened is a burst of tens. A bucket of 40 refilled at 4 a second
// takes any of those and a sustained player pace with room to spare, and stops a load's pre-delete
// (thousands in one second) after 40.
constexpr double kCapacity      = 40.0;
constexpr double kRefillPerSec  = 4.0;
// Once tripped, a slot stays refused until its bucket is half full again (5 s of quiet): a flood that
// outruns the refill would otherwise be let through one token at a time, a trickle of the very churn
// the guard is there to stop.
constexpr double kResumeAt      = kCapacity / 2;
constexpr int    kSlots         = 16;

struct Bucket {
    double   tokens = kCapacity;
    uint64_t lastMs = 0;         // 0 = never metered
    bool     tripped = false;    // refusing until the bucket is back to kResumeAt
    uint32_t refused = 0;        // in the current flood
    uint64_t floodStartMs = 0;   // the first refusal of the current flood
    uint32_t refusedTotal = 0;   // since the process started, for the log
};

Bucket g_buckets[kSlots];

// The refused props to give back, once each: by key, or by eid when the destroy named no key.
struct Restore { std::wstring key; unsigned eid; };
std::deque<Restore> g_restore;
std::unordered_set<std::wstring> g_queued;   // "k:<key>" or "e:<eid>", so a flood names each prop once
constexpr size_t kRestoreCap     = 8192;     // a whole world's worth; past it the rest stay lost to that client
constexpr int    kRestorePerTick = 20;

void Refill_(Bucket& b, uint64_t now) {
    if (b.lastMs != 0 && now > b.lastMs) {
        b.tokens += static_cast<double>(now - b.lastMs) * kRefillPerSec / 1000.0;
        if (b.tokens > kCapacity) b.tokens = kCapacity;
    }
    b.lastMs = now;
}

}  // namespace

bool Admit(int slot, const wchar_t* keyW, unsigned eid) {
    UE_ASSERT_GAME_THREAD("destroy_flood_guard::Admit");
    if (slot <= 0 || slot >= kSlots) return true;  // the host's own slot, or one this table cannot hold
    Bucket& b = g_buckets[slot];
    const uint64_t now = coop::net::NowMs();
    Refill_(b, now);
    const wchar_t* const key = (keyW && *keyW) ? keyW : L"None";
    if (b.tripped && b.tokens >= kResumeAt) {
        b.tripped = false;
        UE_LOGW("destroy_flood_guard: slot %d's destroy flood is over -- %u destroy(s) refused over %.1f s "
                "(%u refused this process); its destroys apply again",
                slot, b.refused, static_cast<double>(now - b.floodStartMs) / 1000.0, b.refusedTotal);
        b.refused = 0;
    }
    if (!b.tripped && b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    if (!b.tripped) {
        b.tripped = true;
        b.floodStartMs = now;
        UE_LOGE("destroy_flood_guard: slot %d commanded more than %.0f prop destroys at once (refill %.0f/s) -- "
                "REFUSING its destroys until it slows down; the host's props stay (first refused key='%ls' "
                "eid=%u). A client's load churn reaching the wire looks like this.",
                slot, kCapacity, kRefillPerSec, key, eid);
    }
    b.tokens = 0.0;  // a refused destroy is not quiet: the refill toward kResumeAt starts over
    ++b.refused;
    ++b.refusedTotal;
    const std::wstring tag = (keyW && *keyW) ? L"k:" + std::wstring(keyW) : L"e:" + std::to_wstring(eid);
    if (g_restore.size() < kRestoreCap && g_queued.insert(tag).second)
        g_restore.push_back(Restore{(keyW && *keyW) ? std::wstring(keyW) : std::wstring(), eid});
    if (b.refused % 200 == 0)
        UE_LOGW("destroy_flood_guard: slot %d -- %u destroy(s) refused so far in this flood (last key='%ls' eid=%u)",
                slot, b.refused, key, eid);
    return false;
}

void Tick() {
    if (g_restore.empty()) return;
    UE_ASSERT_GAME_THREAD("destroy_flood_guard::Tick");
    int expressed = 0, gone = 0;
    for (int n = 0; n < kRestorePerTick && !g_restore.empty(); ++n) {
        const Restore r = std::move(g_restore.front());
        g_restore.pop_front();
        g_queued.erase(!r.key.empty() ? L"k:" + r.key : L"e:" + std::to_wstring(r.eid));
        void* actor = !r.key.empty() ? coop::prop_key_index::Find(r.key) : nullptr;
        if (!actor && r.eid) actor = coop::element::LivePropActor(r.eid);
        if (!actor) { ++gone; continue; }   // destroyed since by a destroy the host did admit
        coop::prop_snapshot::ExpressIncrementalSpawn(actor);
        ++expressed;
    }
    static uint32_t s_said = 0;
    if (expressed && s_said++ < 20)
        UE_LOGI("destroy_flood_guard: %d refused prop(s) given back to the peers (%d gone since, %zu still queued)",
                expressed, gone, g_restore.size());
}

void Reset() {
    g_restore.clear();
    g_queued.clear();
    for (Bucket& b : g_buckets) b = Bucket{};
}

}  // namespace coop::destroy_flood_guard
