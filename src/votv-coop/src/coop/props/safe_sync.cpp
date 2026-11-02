// coop/props/safe_sync.cpp -- see coop/props/safe_sync.h.

#include "coop/props/safe_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_record_gates.h"
#include "coop/props/prop_save_data.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/safe.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::safe_sync {
namespace {

namespace sg = ue_wrap::script_gate;
using ue_wrap::CachedObjRef;

UE_FACT_FN(kRowOpened, "safe_sync", L"prop_safe_C", L"opened", Declared);
constexpr int kTag = 0x53414645;  // 'SAFE'
constexpr float kReachUU = 450.f;
constexpr uint64_t kRequestTtlMs = 5000, kRetryMs = 500;
constexpr size_t kMaxRows = 64;

struct Pending {  // CLIENT: a request not yet sent, or sent and not yet answered
    std::wstring key;
    uint64_t at = 0;
    bool sent = false;
};
struct Publish {  // HOST: a safe whose record is owed to the peers
    CachedObjRef actor;
    uint64_t retryAt = 0;
    int tries = 0;
};

std::atomic<net::Session*> g_session{nullptr};
std::vector<Pending> g_pending;
std::unordered_map<std::wstring, Publish> g_publish;
uint32_t g_sequence = 0, g_world = 0;
uint32_t g_lastRequest[net::kMaxPeers]{};
bool g_watched = false;

uint64_t Now() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
net::Session* Connected() {
    auto* s = g_session.load();
    return s && s->connected() ? s : nullptr;
}
bool IsRole(net::Role r) {
    auto* s = Connected();
    return s && s->role() == r;
}
bool After(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }
void World() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == g_world) return;
    g_world = gen;
    g_pending.clear();
    g_publish.clear();
}

std::wstring KeyOf(void* safe) {
    const std::wstring key = ue_wrap::prop::GetKeyString(safe);
    return key.empty() || key == L"None" || key.size() > 31 ? std::wstring() : key;
}

void OwePublish(void* safe) {
    const std::wstring key = KeyOf(safe);
    if (key.empty() || (g_publish.size() >= kMaxRows && !g_publish.count(key))) return;
    Publish& row = g_publish[key];
    row.actor.Set(safe);
    row.retryAt = 0;
    row.tries = 0;
}

// `opened` runs after the dial's checkPass or a break's `hacked` has already turned the local lock. A client's
// is cancelled -- only the host creates the contents -- and becomes a request; the host's runs and owes the
// peers the safe's record.
sg::Verdict OnOpenedPre(const sg::Call& c) {
    if (!IsRole(net::Role::Client) || !ue_wrap::safe::IsSafe(c.object) || !net_pump::IsInAnnouncedWorld(c.object))
        return sg::Verdict::Run;
    if (!c.fromOurCode) {
        World();
        const std::wstring key = KeyOf(c.object);
        bool pending = false;
        for (const Pending& p : g_pending)
            if (p.key == key) pending = true;
        if (!key.empty() && !pending && g_pending.size() < 8) g_pending.push_back({key, Now(), false});
    }
    return sg::Verdict::Cancel;
}
void OnOpenedPost(const sg::Call& c) {
    if (!IsRole(net::Role::Host) || !ue_wrap::safe::IsSafe(c.object)) return;
    World();
    OwePublish(c.object);
}

// A client's record never moves a safe's lock: the lock moves on the host's `hacked` alone.
bool RefuseClientRecord(void*, uint8_t) { return false; }

}  // namespace

void Install(net::Session* session) {
    g_session.store(session);
    World();
    if (!g_watched) {
        g_watched = sg::WatchName(kRowOpened, kTag, &OnOpenedPre, &OnOpenedPost);
        coop::prop_record_gates::RegisterIntentGate(L"prop_safe_C", &RefuseClientRecord);
        if (!g_watched) UE_LOGW("safe_sync: the watch on prop_safe_C.opened did not register -- each peer opens its own");
    }
}

void Receive(const net::SafeUnlockPayload& p, uint8_t sender) {
    auto* s = Connected();
    if (!s || s->role() != net::Role::Host || sender == 0 || sender >= net::kMaxPeers || !p.sequence) return;
    if (g_lastRequest[sender] && !After(p.sequence, g_lastRequest[sender])) return;
    g_lastRequest[sender] = p.sequence;
    World();
    const std::wstring key = net::StringFromWireKey(p.key);
    void* safe = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    if (!ue_wrap::safe::IsSafe(safe)) {
        UE_LOGI("safe_sync: HOST slot %u asked to open '%ls', which is no safe here", static_cast<unsigned>(sender),
                key.c_str());
        return;
    }
    const auto auth = element::IntentTarget::ForClientIntent(*s, sender, kReachUU).Authorize(safe);
    if (!auth) {
        // The requester saw its dial succeed: its copy gets the host's lock back.
        UE_LOGI("safe_sync: HOST refused slot %u's opening of '%ls' -- out of reach", static_cast<unsigned>(sender),
                key.c_str());
        coop::prop_save_data::PublishToSlot(s, sender, safe, key);
        return;
    }
    CachedObjRef live;
    live.Set(safe);
    if (ue_wrap::safe::Unlock(safe) && live.Get()) {
        OwePublish(safe);
        UE_LOGI("safe_sync: HOST opened '%ls' for slot %u", key.c_str(), static_cast<unsigned>(sender));
    }
}

void Tick() {
    auto* s = Connected();
    if (!s) return;
    World();
    const uint64_t now = Now();
    if (s->role() == net::Role::Client) {
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (now - it->at > kRequestTtlMs) {
                it = g_pending.erase(it);
                continue;
            }
            if (!it->sent) {
                net::SafeUnlockPayload p{};
                net::WireKeyFromString(it->key, p.key);
                if (!++g_sequence) ++g_sequence;
                p.sequence = g_sequence;
                if (s->SendReliable(net::ReliableKind::SafeUnlock, &p, sizeof(p))) {
                    it->sent = true;
                    UE_LOGI("safe_sync: CLIENT asked the host to open '%ls'", it->key.c_str());
                }
            }
            ++it;
        }
        return;
    }
    // HOST: the record lane carries the lock and the password with its own birth ordering and late join;
    // `loadData` restores them without running `opened` again.
    for (auto it = g_publish.begin(); it != g_publish.end();) {
        void* safe = it->second.actor.Get();
        if (!safe || KeyOf(safe) != it->first) {
            it = g_publish.erase(it);
            continue;
        }
        if (now < it->second.retryAt) {
            ++it;
            continue;
        }
        it->second.retryAt = now + kRetryMs;
        if (coop::prop_save_data::Publish(s, safe, it->first)) {
            it = g_publish.erase(it);
            continue;
        }
        if (++it->second.tries >= 10) {  // the record lane keeps its own resend; this only failed to start one
            UE_LOGW("safe_sync: HOST could not publish '%ls' in 10 tries -- the peers keep the lock they had",
                    it->first.c_str());
            it = g_publish.erase(it);
            continue;
        }
        ++it;
    }
}

void OnDisconnect() {
    g_session.store(nullptr);
    g_pending.clear();
    g_publish.clear();
    for (auto& r : g_lastRequest) r = 0;
    g_world = 0;
}

}  // namespace coop::safe_sync
