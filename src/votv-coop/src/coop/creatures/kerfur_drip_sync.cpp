// coop/creatures/kerfur_drip_sync.cpp -- see coop/creatures/kerfur_drip_sync.h.

#include "coop/creatures/kerfur_drip_sync.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::kerfur_drip_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
using coop::element::ElementId;

UE_FACT_FN(kRowUpdateDrip, "kerfur_drip_sync", L"kerfurOmega_C", L"updateDrip", Declared);
constexpr int kTag = 0x4B445250;  // 'KDRP'
constexpr uint64_t kHostResendMs = 5000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false, g_refused = false;
bool g_applying = false;   // our own updateDrip on a peer's list: not sent back

struct NameArray { R::FName* data; int32_t num; int32_t max; };

void* g_kerfurCls = nullptr;
int32_t g_dripOff = -2;
uint64_t g_lastResendMs = 0;
uint64_t g_sent = 0, g_applied = 0, g_unresolved = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Resolve(void* anyKerfur) {
    if (!g_kerfurCls) g_kerfurCls = OI::ClassByName(L"kerfurOmega_C");
    if (g_dripOff == -2) {
        void* cls = g_kerfurCls ? g_kerfurCls : (anyKerfur ? R::ClassOf(anyKerfur) : nullptr);
        if (!cls) return false;
        g_dripOff = R::FindPropertyOffset(cls, L"drip");
        if (g_dripOff < 0) UE_LOGE("[KDRIP] kerfurOmega_C.drip did not resolve -- accessories stay per peer");
    }
    return g_dripOff >= 0;
}

bool IsOmega(void* actor) {
    void* cls = actor ? R::ClassOf(actor) : nullptr;
    if (!g_kerfurCls) g_kerfurCls = OI::ClassByName(L"kerfurOmega_C");
    return cls && g_kerfurCls && R::IsDescendantOfAny(cls, &g_kerfurCls, 1);
}

std::vector<std::string> ReadDrip(void* actor) {
    std::vector<std::string> out;
    const auto* arr = reinterpret_cast<const NameArray*>(reinterpret_cast<const uint8_t*>(actor) + g_dripOff);
    if (!arr->data || arr->num <= 0 || arr->num > 64) return out;
    for (int32_t i = 0; i < arr->num; ++i) {
        const std::wstring w = R::ToString(arr->data[i]);
        std::string n;
        for (wchar_t c : w) n.push_back(static_cast<char>(c & 0x7f));
        out.push_back(std::move(n));
    }
    return out;
}

bool WriteDrip(void* actor, const std::vector<std::string>& names) {
    auto* arr = reinterpret_cast<NameArray*>(reinterpret_cast<uint8_t*>(actor) + g_dripOff);
    const int32_t n = static_cast<int32_t>(names.size());
    if (n > 0 && (!arr->data || arr->max < n)) {
        auto* buf = static_cast<R::FName*>(R::EngineAlloc(sizeof(R::FName) * static_cast<size_t>(n)));
        if (!buf) return false;
        if (arr->data) R::EngineFree(arr->data);
        arr->data = buf;
        arr->max = n;
    }
    for (int32_t i = 0; i < n; ++i) {
        const std::string& s = names[static_cast<size_t>(i)];
        arr->data[i] = ue_wrap::fname_utils::StringToFName(std::wstring(s.begin(), s.end()));
    }
    arr->num = n;
    return true;
}

// The npc lane's eid for this peer's Omega: the element a mirror (a client's) or the host's own carries.
ElementId EidOf(void* actor) {
    std::vector<coop::element::Npc*> elems;
    coop::element::MirrorManager<coop::element::Npc>::Instance().Snapshot(elems);
    ElementId any = coop::element::kInvalidId;
    for (coop::element::Npc* el : elems) {
        if (!el || el->GetActor() != actor) continue;
        if (el->IsMirror()) return el->GetId();
        any = el->GetId();
    }
    return any;
}

bool Send(coop::net::Session& s, void* actor, const char* why) {
    const ElementId eid = EidOf(actor);
    if (eid == coop::element::kInvalidId) return false;
    const std::vector<std::string> names = ReadDrip(actor);
    coop::net::KerfurDripPayload p{};
    p.eid = static_cast<uint32_t>(eid);
    for (const std::string& n : names) {
        if (p.count >= coop::net::kKerfurDripMax) break;
        if (n.empty() || n.size() >= sizeof(p.names[0])) continue;
        std::memcpy(p.names[p.count], n.data(), n.size());
        ++p.count;
    }
    const size_t len = offsetof(coop::net::KerfurDripPayload, names) + p.count * sizeof(p.names[0]);
    if (!s.SendReliable(coop::net::ReliableKind::KerfurDrip, &p, len)) return false;
    ++g_sent;
    if (why) UE_LOGI("[KDRIP] Omega eid=%u wears %u accessories -- sent (%s)", p.eid, p.count, why);
    return true;
}

void OnUpdateDripPost(const sg::Call& call) {
    if (g_applying) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || !call.object || !Resolve(call.object)) return;
    if (s->role() == coop::net::Role::Client && !coop::net_pump::IsInAnnouncedWorld(call.object)) return;
    Send(*s, call.object, "dressed here");
}

struct Collect { std::vector<void*>* out; };

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched || g_refused) return;
    if (sg::WatchName(kRowUpdateDrip, kTag, nullptr, &OnUpdateDripPost)) { g_watched = true; return; }
    g_refused = true;
    UE_LOGE("[KDRIP] the script-body gate refused the updateDrip watch -- an Omega's accessories stay per peer");
}

void Tick() {
    if (!g_watched && !g_refused) Install(g_session.load(std::memory_order_acquire));
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || s->role() != coop::net::Role::Host) return;
    const uint64_t now = NowMs();
    if (now - g_lastResendMs < kHostResendMs) return;
    g_lastResendMs = now;
    if (!g_kerfurCls) g_kerfurCls = OI::ClassByName(L"kerfurOmega_C");
    if (!g_kerfurCls || !Resolve(nullptr)) return;
    std::vector<void*> omegas;
    Collect c{&omegas};
    // The skins are data-only subclasses, so every live actor whose class descends from the Omega counts.
    OI::ForEachInstance(g_kerfurCls, [](void* ctx, void* obj, int32_t) {
        static_cast<Collect*>(ctx)->out->push_back(obj);
    }, &c);
    std::vector<coop::element::Npc*> elems;
    coop::element::MirrorManager<coop::element::Npc>::Instance().Snapshot(elems);
    for (coop::element::Npc* el : elems) {
        void* a = el ? el->GetActor() : nullptr;
        if (a && R::IsLiveByIndex(a, el->GetInternalIdx()) && IsOmega(a)) {
            bool have = false;
            for (void* o : omegas) have = have || o == a;
            if (!have) omegas.push_back(a);
        }
    }
    for (void* a : omegas) {
        const std::wstring nm = R::ToString(R::NameOf(a));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        Send(*s, a, nullptr);
    }
}

void OnKerfurDrip(const coop::net::KerfurDripPayload& p, int senderSlot) {
    auto* el = coop::element::MirrorManager<coop::element::Npc>::Instance().Get(static_cast<ElementId>(p.eid));
    void* actor = el ? el->GetActor() : nullptr;
    if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx()) || !IsOmega(actor) || !Resolve(actor)) {
        if (++g_unresolved <= 5)
            UE_LOGW("[KDRIP] slot %d's accessories for Omega eid=%u -- no such Omega here", senderSlot, p.eid);
        return;
    }
    std::vector<std::string> names;
    for (int i = 0; i < p.count && i < coop::net::kKerfurDripMax; ++i) names.emplace_back(p.names[i]);
    if (ReadDrip(actor) == names) return;
    if (!WriteDrip(actor, names)) return;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"updateDrip");
    g_applying = true;
    const bool ran = fn && R::CallFunction(actor, fn, nullptr);
    g_applying = false;
    ++g_applied;
    UE_LOGI("[KDRIP] Omega eid=%u dressed in slot %d's %zu accessories%s", p.eid, senderSlot, names.size(),
            ran ? "" : " (updateDrip did not run)");
    // The host passes a client's change on: the relay carries it to the other clients, and the host's own
    // copy is the one the save and the next resend read.
}

void OnDisconnect() {
    if (g_sent || g_applied || g_unresolved)
        UE_LOGI("[KDRIP] session end -- lists sent=%llu applied=%llu unresolved=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_unresolved));
    g_sent = g_applied = g_unresolved = 0;
    g_lastResendMs = 0;
}

}  // namespace coop::kerfur_drip_sync
