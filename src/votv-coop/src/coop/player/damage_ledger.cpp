// coop/player/damage_ledger.cpp -- see coop/player/damage_ledger.h.
#include "coop/player/damage_ledger.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::player::damage_ledger {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// A third watch on the verb (player_damage refuses it on puppets, wisp_attack_sync reads it); its own tag.
UE_FACT_FN(kRowAddPlayerDamage, "damage_ledger", nullptr, L"Add Player Damage", AnyClass);
constexpr int kTag = 0x444D4C47;  // 'DMLG'

struct Hit {
    ULONGLONG    ms = 0;
    float        amount = 0.f;
    float        healthBefore = 0.f;
    std::wstring source, caller;
    std::string  detail;   // where the hit landed and the player's fall velocity: tells a fall from a blow
};
constexpr int kRing = 12;
Hit g_ring[kRing];
int g_next = 0, g_count = 0;

// The kinds already said (caller + source), so a burn's once-a-second hit is logged once, then counted.
constexpr int kKinds = 48;
std::wstring g_kinds[kKinds];
uint32_t     g_kindHits[kKinds] = {};
int          g_kindCount = 0;
uint32_t     g_puppetHits = 0;
// This machine's player, published by Tick on the game thread (Registry::Local is game-thread only).
std::atomic<void*> g_local{nullptr};

std::wstring ClassOrNull(void* obj) { return obj ? R::ClassNameOf(obj) : std::wstring(L"null"); }

int KindIndex(const std::wstring& k, bool* isNew) {
    for (int i = 0; i < g_kindCount; ++i)
        if (g_kinds[i] == k) { *isNew = false; return i; }
    *isNew = true;
    if (g_kindCount >= kKinds) return -1;
    g_kinds[g_kindCount] = k;
    return g_kindCount++;
}

void LogRing(const char* why) {
    const ULONGLONG now = ::GetTickCount64();
    UE_LOGW("[DEATH-CAUSE] %s -- the last %d hits on this machine's player, oldest first:", why,
            g_count < kRing ? g_count : kRing);
    const int n = g_count < kRing ? g_count : kRing;
    for (int i = 0; i < n; ++i) {
        const Hit& h = g_ring[(g_next - n + i + kRing) % kRing];
        UE_LOGW("[DEATH-CAUSE]   -%.1fs  %.2f damage (health was %.1f)  source=%ls  by %ls  %s",
                (now - h.ms) / 1000.0, h.amount, h.healthBefore, h.source.c_str(), h.caller.c_str(),
                h.detail.c_str());
    }
    for (int i = 0; i < g_kindCount; ++i)
        UE_LOGW("[DEATH-CAUSE]   this session: %u x %ls", g_kindHits[i], g_kinds[i].c_str());
}

sg::Verdict OnDamagePre(const sg::Call& c) {
    if (!c.locals || !c.function || !c.object) return sg::Verdict::Run;
    auto& reg = coop::players::Registry::Get();
    if (reg.IsPuppet(c.object)) {
        ++g_puppetHits;  // refused by player_damage; only counted here
        return sg::Verdict::Run;
    }
    if (c.object != g_local.load(std::memory_order_acquire)) return sg::Verdict::Run;

    static R::ParamOffset sAmount(L"Damage");
    static R::ParamOffset sSource(L"Source");
    static R::ParamOffset sWhere(L"damageLocation");
    float amount = 0.f;
    void* source = nullptr;
    const int32_t ao = sAmount.Of(c.function);
    const int32_t so = sSource.Of(c.function);
    if (ao >= 0) std::memcpy(&amount, c.locals + ao, sizeof(amount));
    if (so >= 0) std::memcpy(&source, c.locals + so, sizeof(source));
    float where[3] = {0.f, 0.f, 0.f};
    if (const int32_t wo = sWhere.Of(c.function); wo >= 0) std::memcpy(where, c.locals + wo, sizeof(where));
    // The game's fall damage reads fallVeloc and lands at (0,0,0); a blow lands where it struck.
    static int32_t sFallOff = -2;
    if (sFallOff == -2) sFallOff = R::FindPropertyOffset(R::ClassOf(c.object), L"fallVeloc");
    float fall[3] = {0.f, 0.f, 0.f};
    if (sFallOff >= 0) std::memcpy(fall, static_cast<uint8_t*>(c.object) + sFallOff, sizeof(fall));
    char detail[160];
    std::snprintf(detail, sizeof(detail), "at=(%.0f,%.0f,%.0f) fallVeloc=(%.0f,%.0f,%.0f) source%s",
                  where[0], where[1], where[2], fall[0], fall[1], fall[2],
                  source == c.object ? "=self" : (source ? "=other" : "=none"));
    float health = 0.f;
    ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &health);

    Hit& h = g_ring[g_next];
    h.ms = ::GetTickCount64();
    h.amount = amount;
    h.healthBefore = health;
    h.source = ClassOrNull(source);
    h.detail = detail;
    h.caller = ClassOrNull(c.callerObject) + L"." +
               (c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)) : std::wstring(L"<ProcessEvent>"));
    g_next = (g_next + 1) % kRing;
    ++g_count;

    bool isNew = false;
    const int k = KindIndex(h.caller + L" source=" + h.source, &isNew);
    if (k >= 0) ++g_kindHits[k];
    if (isNew)
        UE_LOGI("[DMG-SRC] first hit of its kind on this machine's player: %.2f damage (health %.1f) source=%ls by %ls %s",
                amount, health, h.source.c_str(), h.caller.c_str(), detail);
    else if (amount >= 25.f)
        UE_LOGI("[DMG-SRC] a heavy hit: %.2f damage (health %.1f) source=%ls by %ls %s",
                amount, health, h.source.c_str(), h.caller.c_str(), detail);
    if (amount > 0.f && health > 0.f && health - amount <= 0.f) LogRing("a lethal hit");
    return sg::Verdict::Run;
}

}  // namespace

void Tick() {
    g_local.store(coop::players::Registry::Get().Local(), std::memory_order_release);
    static bool sWatched = false;
    if (sWatched) return;
    sWatched = true;
    if (sg::WatchName(kRowAddPlayerDamage, kTag, &OnDamagePre, nullptr))
        UE_LOGI("damage_ledger: every hit on this machine's player is recorded; a lethal one logs [DEATH-CAUSE]");
    else
        UE_LOGW("damage_ledger: could not watch '%ls'", kRowAddPlayerDamage.Name());
}

}  // namespace coop::player::damage_ledger
