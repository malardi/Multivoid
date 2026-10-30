// coop/props/pry_crowbar_sync.cpp -- see coop/props/pry_crowbar_sync.h.

#include "coop/props/pry_crowbar_sync.h"

#include "coop/element/element.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::pry_crowbar_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace PR = ue_wrap::prop;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowBegin, "pry_crowbar_sync", L"pryingCrowbar_C", L"begin", Declared);
UE_FACT_FN(kRowEnd,   "pry_crowbar_sync", L"pryingCrowbar_C", L"end", Declared);
constexpr int kTagBegin = 0x50434231;  // 'PCB1'
constexpr int kTagEnd   = 0x50434230;  // 'PCB0'

// A pry is a few seconds of mouse work; a peer's crowbar still out after this lost its end (the peer left).
constexpr uint64_t kShownMaxMs = 60000;
constexpr int kMaxShown = 8;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_beginWatched = false, g_endWatched = false, g_refused = false;

int32_t g_hiddenOff = -2;
uint8_t g_hiddenMask = 0;

struct Shown { void* actor; int32_t idx; uint64_t untilMs; };
Shown g_shown[kMaxShown] = {};
int g_shownCount = 0;

uint64_t g_sent = 0, g_applied = 0, g_unresolved = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// AActor.bHidden, read from its bit.
bool ReadHidden(void* actor, bool& hidden) {
    if (g_hiddenOff == -2) {
        int32_t off = -1;
        uint8_t mask = 0;
        g_hiddenOff = R::FindBoolProperty(R::ClassOf(actor), L"bHidden", off, mask) ? off : -1;
        g_hiddenMask = mask;
    }
    if (g_hiddenOff < 0 || !g_hiddenMask) return false;
    hidden = (reinterpret_cast<const uint8_t*>(actor)[g_hiddenOff] & g_hiddenMask) != 0;
    return true;
}

void Send(void* child, bool visible) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || !child) return;
    if (s->role() == coop::net::Role::Client && !coop::net_pump::IsInAnnouncedWorld(child)) return;
    std::wstring comp;
    void* plank = E::ParentActorOf(child, &comp);
    if (!plank || comp.empty() || comp.size() >= sizeof(coop::net::PryCrowbarPayload::component)) {
        UE_LOGW("[PRYBAR] crowbar %p has no plank or component name -- its %s stays here", child,
                visible ? "show" : "hide");
        return;
    }
    coop::net::PryCrowbarPayload p{};
    const std::wstring key = PR::GetInteractableKeyString(plank);
    if (!key.empty() && key != L"None") coop::net::WireKeyFromString(key, p.plankKey);
    const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(plank);
    p.plankEid = eid == coop::element::kInvalidId ? 0u : static_cast<uint32_t>(eid);
    if (p.plankKey.len == 0 && p.plankEid == 0) {
        UE_LOGW("[PRYBAR] plank %p has neither key nor eid -- its crowbar's %s stays here", plank,
                visible ? "show" : "hide");
        return;
    }
    for (size_t i = 0; i < comp.size(); ++i) p.component[i] = static_cast<char>(comp[i] & 0x7f);
    p.visible = visible ? 1 : 0;
    if (!s->SendReliable(coop::net::ReliableKind::PryCrowbar, &p, sizeof(p))) return;
    if (++g_sent <= 10)
        UE_LOGI("[PRYBAR] the crowbar '%ls' of plank key='%ls' eid=%u %s here -- sent", comp.c_str(), key.c_str(),
                p.plankEid, visible ? "came out" : "went back");
}

void OnBeginPost(const sg::Call& call) {
    // begin refuses (its failed out) when the pivot is blocked or the hand holds no crowbar; then the child
    // stays hidden, and only a shown child is sent.
    bool hidden = true;
    if (!call.object || !ReadHidden(call.object, hidden) || hidden) return;
    Send(call.object, true);
}

sg::Verdict OnEndPre(const sg::Call& call) {
    bool hidden = true;
    if (call.object && ReadHidden(call.object, hidden) && !hidden) Send(call.object, false);
    return sg::Verdict::Run;
}

struct FindCtx { void* plank; const std::wstring* comp; void* found; };

void* FindChild(void* plank, const std::wstring& comp) {
    static void* sCls = nullptr;
    if (!sCls) sCls = OI::ClassByName(L"pryingCrowbar_C");
    if (!sCls) return nullptr;
    FindCtx ctx{plank, &comp, nullptr};
    OI::ForEachInstance(sCls, [](void* c, void* obj, int32_t) {
        auto* f = static_cast<FindCtx*>(c);
        if (f->found) return;
        std::wstring name;
        if (E::ParentActorOf(obj, &name) == f->plank && name == *f->comp) f->found = obj;
    }, &ctx);
    return ctx.found;
}

void Remember(void* actor, bool visible) {
    for (int i = 0; i < g_shownCount; ++i) {
        if (g_shown[i].actor != actor) continue;
        if (visible) { g_shown[i].untilMs = NowMs() + kShownMaxMs; return; }
        g_shown[i] = g_shown[--g_shownCount];
        return;
    }
    if (visible && g_shownCount < kMaxShown)
        g_shown[g_shownCount++] = Shown{actor, R::InternalIndexOf(actor), NowMs() + kShownMaxMs};
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_refused) return;
    if (!g_beginWatched) g_beginWatched = sg::WatchName(kRowBegin, kTagBegin, nullptr, &OnBeginPost);
    if (!g_endWatched) g_endWatched = sg::WatchName(kRowEnd, kTagEnd, &OnEndPre, nullptr);
    if (!g_beginWatched || !g_endWatched) {
        g_refused = true;
        UE_LOGE("[PRYBAR] the script-body gate refused a crowbar watch (begin=%d end=%d) -- a pried plank's "
                "crowbar shows on the prier's screen alone", g_beginWatched ? 1 : 0, g_endWatched ? 1 : 0);
    }
}

void Tick() {
    if (!g_refused && (!g_beginWatched || !g_endWatched)) Install(g_session.load(std::memory_order_acquire));
    if (g_shownCount == 0) return;
    const uint64_t now = NowMs();
    int w = 0;
    for (int i = 0; i < g_shownCount; ++i) {
        const Shown sh = g_shown[i];
        if (!sh.actor || !R::IsLiveByIndex(sh.actor, sh.idx)) continue;
        if (now >= sh.untilMs) {
            E::SetActorHiddenInGame(sh.actor, true);
            UE_LOGI("[PRYBAR] a peer's crowbar %p was out %llu s with no end -- hidden", sh.actor,
                    static_cast<unsigned long long>(kShownMaxMs / 1000));
            continue;
        }
        g_shown[w++] = sh;
    }
    g_shownCount = w;
}

void OnPryCrowbar(const coop::net::PryCrowbarPayload& p) {
    const std::wstring key = coop::remote_prop::KeyToWString(p.plankKey);
    void* plank = nullptr;
    if (!key.empty() && key != L"None") plank = coop::prop_key_index::Find(key);
    if (!plank && p.plankEid != 0) {
        auto* el = coop::element::MirrorManager<coop::element::Prop>::Instance().Get(
            static_cast<coop::element::ElementId>(p.plankEid));
        void* a = el ? el->GetActor() : nullptr;
        if (a && R::IsLiveByIndex(a, el->GetInternalIdx())) plank = a;
    }
    std::wstring comp;
    for (size_t i = 0; i < sizeof(p.component) && p.component[i]; ++i)
        comp.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.component[i])));
    void* child = plank ? FindChild(plank, comp) : nullptr;
    if (!child) {
        if (++g_unresolved <= 10)
            UE_LOGW("[PRYBAR] a peer's crowbar '%ls' of plank key='%ls' eid=%u -- %s here", comp.c_str(), key.c_str(),
                    p.plankEid, plank ? "the plank has no such crowbar" : "no such plank");
        return;
    }
    E::SetActorHiddenInGame(child, p.visible == 0);
    Remember(child, p.visible != 0);
    if (++g_applied <= 10)
        UE_LOGI("[PRYBAR] a peer's crowbar '%ls' of plank key='%ls' %s here", comp.c_str(), key.c_str(),
                p.visible ? "shown" : "hidden");
}

void OnDisconnect() {
    for (int i = 0; i < g_shownCount; ++i)
        if (g_shown[i].actor && R::IsLiveByIndex(g_shown[i].actor, g_shown[i].idx))
            E::SetActorHiddenInGame(g_shown[i].actor, true);
    g_shownCount = 0;
    if (g_sent || g_applied || g_unresolved)
        UE_LOGI("[PRYBAR] session end -- crowbars sent=%llu shown/hidden for peers=%llu unresolved=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_unresolved));
    g_sent = g_applied = g_unresolved = 0;
}

}  // namespace coop::pry_crowbar_sync
