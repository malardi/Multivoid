// coop/dev/stat_order_drill.cpp -- see coop/dev/stat_order_drill.h.

#include "coop/dev/stat_order_drill.h"
#include "coop/dev/rearm.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/stat_orders.h"
#include "coop/session/player_handshake.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace coop::dev::stat_order_drill {
namespace {

namespace SO = coop::stat_orders;
namespace V = ue_wrap::vitals;
using coop::net::Session;

enum class Arm : uint8_t { Off, On, Red, RedJoin };

Arm ArmNow() {
    static Arm arm{};
    static uint32_t armSeen = ~0u;  // re-read after the lab re-arms the drills
    coop::dev::rearm::Latched(arm, armSeen, [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::stat_order_drill);
        return v == "on" ? Arm::On : v == "red" ? Arm::Red : v == "redjoin" ? Arm::RedJoin : Arm::Off;
    });
    return arm;
}

constexpr int kSteps = 12;
constexpr float kTolerance = 0.001f;
constexpr float kFoodSet = 42.f;
constexpr const char* kHostEffect = "vaccine_a";
constexpr const char* kJoinerEffect = "sleepy";
constexpr uint32_t kAllRows = (1u << coop::net::kStatRows) - 1u;

int g_step = 1;
bool g_inFlight = false;   // a send of this step waits for its answer
bool g_finished = false;
int g_run = 0;             // bumped at a session's end: an answer of an earlier run is ignored
int g_bad = 0;             // MISMATCH and UNMEASURABLE steps
int g_ok = 0;
uint8_t g_slot = 0;        // the client the drill orders
float g_food = 0.f;        // step 2's reading, put back at step 9
float g_sleep = 0.f;       // and step 10

bool Near(float a, float b) { return std::fabs(a - b) <= kTolerance; }

const char* SentText(SO::Sent s) {
    switch (s) {
        case SO::Sent::Ok:         return "Ok";
        case SO::Sent::NotHost:    return "NotHost";
        case SO::Sent::BadName:    return "BadName";
        case SO::Sent::NotReady:   return "NotReady";
        case SO::Sent::NoGuid:     return "NoGuid";
        case SO::Sent::Busy:       return "Busy";
        case SO::Sent::SendFailed: return "SendFailed";
    }
    return "Unknown";
}

void Finish() {
    g_finished = true;
    UE_LOGI("[STAT-ORDER] DONE bad=%d ok=%d", g_bad, g_ok);
}

void Pass(const std::string& what, SO::Result r, float now, bool stored) {
    UE_LOGI("[STAT-ORDER] step %d OK: %s -> %s now=%.3f stored=%d", g_step, what.c_str(), SO::ResultText(r), now,
            stored ? 1 : 0);
    ++g_ok;
    if (++g_step > kSteps) Finish();
}

void Mismatch(const std::string& what, SO::Result r, float now, bool stored, const std::string& why) {
    UE_LOGE("[STAT-ORDER] step %d MISMATCH: %s -> %s now=%.3f stored=%d (%s)", g_step, what.c_str(),
            SO::ResultText(r), now, stored ? 1 : 0, why.c_str());
    ++g_bad;
    Finish();
}

void Unmeasurable(const std::string& why) {
    UE_LOGW("[STAT-ORDER] step %d UNMEASURABLE: %s", g_step, why.c_str());
    ++g_bad;
    Finish();
}

// A send the transport or the table refused is a MISMATCH of that step.
void SendRefused(const std::string& what, SO::Sent sent) {
    g_inFlight = false;
    Mismatch(what, SO::Result::Left, 0.f, false, std::string("the send was ") + SentText(sent));
}

// What an order's answer must be.
struct Expect {
    SO::Result result;
    bool checkNow;
    float now;
    bool wantStored;
};

void JudgeOrder(const std::string& what, const Expect& e, const SO::OrderAnswer& a) {
    if (a.result != e.result) {
        const bool broken = a.result == SO::Result::Left || a.result == SO::Result::Malformed;
        return Mismatch(what, a.result, a.valueNow, a.stored,
                        std::string(broken ? "the answer was lost: " : "expected ") + SO::ResultText(e.result));
    }
    if (e.checkNow && !Near(a.valueNow, e.now)) return Mismatch(what, a.result, a.valueNow, a.stored, "the value in force is not the expected one");
    if (e.wantStored && !a.stored) return Mismatch(what, a.result, a.valueNow, a.stored, "the host's stored profile does not hold it");
    Pass(what, a.result, a.valueNow, a.stored);
}

// Send one order and judge its answer. A slot-0 answer arrives before SendOrder returns.
void RunOrder(uint8_t slot, const SO::Order& order, std::string what, Expect e) {
    const int run = g_run;
    g_inFlight = true;
    const SO::Sent sent = SO::SendOrder(slot, order, [run, what, e](const SO::OrderAnswer& a) {
        if (run != g_run) return;
        g_inFlight = false;
        JudgeOrder(what, e, a);
    });
    if (sent != SO::Sent::Ok) SendRefused(what, sent);
}

using QueryJudge = void (*)(SO::Result, const SO::TableView&);

void RunQuery(uint8_t slot, const char* what, QueryJudge judge) {
    const int run = g_run;
    g_inFlight = true;
    const SO::Sent sent = SO::SendQuery(slot, [run, judge](SO::Result r, const SO::TableView& v) {
        if (run != g_run) return;
        g_inFlight = false;
        judge(r, v);
    });
    if (sent != SO::Sent::Ok) SendRefused(what, sent);
}

// The first entry of `name` in a query's view, or null.
const SO::EffectView* Find(const SO::TableView& v, const char* name) {
    for (const SO::EffectView& e : v.effects)
        if (_stricmp(e.name.c_str(), name) == 0) return &e;
    return nullptr;
}

float Row(const SO::TableView& v, V::Field f) { return v.values[static_cast<int>(f)]; }

bool RowRead(const SO::TableView& v, V::Field f) { return (v.validMask >> static_cast<int>(f)) & 1u; }

// Step 2: the first read of the joiner. Its answer carries the joiner's effects, the measurement of
// whether a joiner's world holds the host's effect from before it joined.
void JudgeFirstQuery(SO::Result r, const SO::TableView& v) {
    const char* what = "query the whole table";
    if (r == SO::Result::NotReady) return Unmeasurable("the joiner's world was changing");
    if (r != SO::Result::Applied) return Mismatch(what, r, 0.f, false, "expected Applied");
    UE_LOGI("[STAT-ORDER] query health=%.3f food=%.3f sleep=%.3f effects=%u", Row(v, V::Field::Health),
            Row(v, V::Field::Food), Row(v, V::Field::Sleep), static_cast<unsigned>(v.effectTotal));
    std::string names;
    for (const SO::EffectView& e : v.effects) names += (names.empty() ? "" : ", ") + e.name;
    UE_LOGI("[STAT-ORDER] joiner effects: %s", names.empty() ? "none" : names.c_str());
    if (v.validMask != kAllRows) return Mismatch(what, r, 0.f, false, "not every row read");
    if (Find(v, kJoinerEffect)) return Unmeasurable("sleepy already active");
    // The reply carries the first five entries only, so a vaccine_a absent from a cut list proves nothing.
    if (static_cast<size_t>(v.effectTotal) > v.effects.size()) return Unmeasurable("the effect list is cut");
    // The joiner's world is built without the host's effects: its save object has them reset out
    // at the join (OnSaveObjectReady). `redjoin` expects the opposite, so on a fixed build it prints the failing line.
    const bool inherited = Find(v, kHostEffect) != nullptr;
    const bool redJoin = ArmNow() == Arm::RedJoin;
    if (inherited && !redJoin)
        return Mismatch(what, r, 0.f, false, "the joiner inherited the host's effect");
    if (!inherited && redJoin)
        return Mismatch(what, r, 0.f, false, "redjoin: the joiner did not inherit the host's effect");
    g_food = Row(v, V::Field::Food);
    g_sleep = Row(v, V::Field::Sleep);
    Pass(what, r, g_food, false);
}

// Step 6: the effect step 5 added, listed live with its strength and the seconds left.
void JudgeEffectListed(SO::Result r, const SO::TableView& v) {
    const char* what = "query the added sleepy";
    if (r != SO::Result::Applied) return Mismatch(what, r, 0.f, false, "expected Applied");
    const SO::EffectView* e = Find(v, kJoinerEffect);
    if (!e) return Mismatch(what, r, 0.f, false, "no sleepy entry listed");
    if (!e->live || !Near(e->strength, 1.f) || !(e->time > 25.f && e->time <= 30.f + kTolerance))
        return Mismatch(what, r, e->time, false, "the entry is not live with strength 1 and 25 to 30 seconds left");
    Pass(what, r, e->time, false);
}

// Step 8: the effect is gone, and the food still stands where step 3 left it (the drain runs
// between answers).
void JudgeAfterRemove(SO::Result r, const SO::TableView& v) {
    const char* what = "query after the removal";
    if (r != SO::Result::Applied) return Mismatch(what, r, 0.f, false, "expected Applied");
    if (!RowRead(v, V::Field::Food)) return Mismatch(what, r, 0.f, false, "food did not read");
    const float food = Row(v, V::Field::Food);
    if (!(food >= kFoodSet - 1.f && food <= kFoodSet + kTolerance))
        return Mismatch(what, r, food, false, "food is not within 41 to 42");
    if (Find(v, kJoinerEffect)) return Mismatch(what, r, food, false, "sleepy is still listed");
    Pass(what, r, food, false);
}

// The host's readiness: one row of each owner reads.
bool HostReady() {
    float v = 0.f;
    return V::Read(V::Field::Health, &v) && V::Read(V::Field::Air, &v) && V::Read(V::Field::Dreaming, &v);
}

// The first client slot that is world-ready with a proved GUID, or 0.
uint8_t ReadyClient(const Session& s) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (s.IsSlotConnected(slot) && s.IsSlotWorldReady(slot) && !coop::player_handshake::GuidForSlot(slot).empty())
            return slot;
    return 0;
}

SO::Order SetOrder(V::Field row, float value) {
    return SO::Order{SO::Op::SetStat, row, value, std::string(), 0.f, 0.f};
}

SO::Order EffectOrder(SO::Op op, const char* name, float strength, float seconds) {
    return SO::Order{op, V::Field::Health, 0.f, name, strength, seconds};
}

}  // namespace

void Tick(Session* session) {
    if (ArmNow() == Arm::Off || g_finished || g_inFlight || !session || !session->running()) return;
    if (session->role() != coop::net::Role::Host) return;
    SO::Install(session);   // the pump ticks the drills before its per-tick Install of the lanes
    const bool red = ArmNow() == Arm::Red;
    switch (g_step) {
    case 1: {
        if (!HostReady()) return;
        for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot)
            if (session->IsSlotConnected(slot)) return Unmeasurable("a joiner was already connected");
        RunOrder(0, EffectOrder(SO::Op::AddEffect, kHostEffect, 0.f, 999.f), "host adds vaccine_a",
                 Expect{SO::Result::Applied, true, 1.f, false});
        return;
    }
    case 2:
        g_slot = ReadyClient(*session);
        if (g_slot == 0) return;
        RunQuery(g_slot, "query the whole table", &JudgeFirstQuery);
        return;
    case 3:
        RunOrder(g_slot, SetOrder(V::Field::Food, kFoodSet), "set food 42",
                 Expect{SO::Result::Applied, true, red ? kFoodSet + 1.f : kFoodSet, true});
        return;
    case 4:
        RunOrder(g_slot, SetOrder(V::Field::Dead, 1.f), "set the read-only Dead",
                 Expect{SO::Result::Refused, false, 0.f, false});
        return;
    case 5:
        RunOrder(g_slot, EffectOrder(SO::Op::AddEffect, kJoinerEffect, 1.f, 30.f), "add sleepy",
                 Expect{SO::Result::Applied, true, 1.f, false});
        return;
    case 6:
        RunQuery(g_slot, "query the added sleepy", &JudgeEffectListed);
        return;
    case 7:
        RunOrder(g_slot, EffectOrder(SO::Op::RemoveEffect, kJoinerEffect, 0.f, 0.f), "remove sleepy",
                 Expect{SO::Result::Applied, true, 0.f, false});
        return;
    case 8:
        RunQuery(g_slot, "query after the removal", &JudgeAfterRemove);
        return;
    case 9:
        RunOrder(g_slot, SetOrder(V::Field::Food, g_food), "put food back",
                 Expect{SO::Result::Applied, false, 0.f, true});
        return;
    case 10:
        RunOrder(g_slot, SetOrder(V::Field::Sleep, g_sleep), "put sleep back",
                 Expect{SO::Result::Applied, false, 0.f, true});
        return;
    case 11:
        RunOrder(0, EffectOrder(SO::Op::RemoveEffect, kHostEffect, 0.f, 0.f), "host removes vaccine_a",
                 Expect{SO::Result::Applied, true, 0.f, false});
        return;
    case 12: {
        const int left = SO::PendingCount(g_slot);
        if (left == 0) Pass("nothing left unanswered", SO::Result::Applied, 0.f, false);
        else Mismatch("nothing left unanswered", SO::Result::Applied, static_cast<float>(left), false,
                      "orders or queries still wait for an answer");
        return;
    }
    default:
        return;
    }
}

void OnDisconnect() {
    ++g_run;
    g_step = 1;
    g_inFlight = false;
    g_finished = false;
    g_bad = 0;
    g_ok = 0;
    g_slot = 0;
    g_food = 0.f;
    g_sleep = 0.f;
}

}  // namespace coop::dev::stat_order_drill
