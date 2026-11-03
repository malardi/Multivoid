// coop/session/desync_watch.cpp -- see coop/session/desync_watch.h.

#include "coop/session/desync_watch.h"

#include "coop/comms/chat_feed.h"
#include "l10n/l10n.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

namespace coop::desync_watch {
namespace {

constexpr uint8_t  kOpDigest = 0, kOpAnswer = 1;
constexpr uint64_t kEveryMs = 5000;
constexpr int      kRunToSay = 3;  // digests out of tolerance in a row before an episode is said

std::atomic<net::Session*> g_session{nullptr};
uint64_t g_next = 0;
uint32_t g_seq = 0;

// One comparison's run: the host keeps one per client slot, a client one for the host.
struct Run {
    int  outOf = 0;     // digests out of tolerance in a row
    bool said = false;  // this episode was said
};
Run g_runs[net::kMaxPeers];

uint64_t Now() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t LocalProps() { return static_cast<uint32_t>(coop::prop_key_index::Size()); }

// A client's world is judged only once it is settled and no join is loading it.
bool ClientSettled() {
    return coop::net_pump::IsWorldSettled() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// A hungry, tired player's hallucinations spawn and vanish on both sides on their own clocks, so two
// worlds in step wander apart by a dozen or more; a lost or mass-destroyed world differs by hundreds.
uint32_t Tolerance(uint32_t a, uint32_t b) {
    const uint32_t big = a > b ? a : b;
    const uint32_t pct = big / 100;  // one percent
    return pct > 25 ? pct : 25;
}

// `slot`: the run's index (the client's slot on the host, 0 on a client). `host`/`client`: the two counts.
void Judge(int slot, uint32_t host, uint32_t client, bool onHost) {
    Run& r = g_runs[slot];
    const uint32_t diff = host > client ? host - client : client - host;
    if (diff <= Tolerance(host, client)) {
        if (r.said) UE_LOGI("[DESYNC] slot %d back in step: host %u props, client %u", slot, host, client);
        r = Run{};
        return;
    }
    if (++r.outOf < kRunToSay || r.said) return;
    r.said = true;
    UE_LOGW("[DESYNC] slot %d's world parted from the host's: host %u keyed props, client %u (%d digests in a row)",
            slot, host, client, r.outOf);
    char line[256];
    const char* fmt = onHost ? l10n::T("World out of sync with player %1$d: you have %2$u items, they have %3$u. "
                                       "F1 > Report a bug saves both logs.")
                             : l10n::T("World out of sync with the host: the host has %1$u items, you have %2$u. "
                                       "Tell the host -- their report holds your log too.");
    const int n = onHost ? l10n::Fmt(line, sizeof(line), fmt, slot, host, client)
                         : l10n::Fmt(line, sizeof(line), fmt, host, client);
    if (n >= 0) coop::chat_feed::Push(std::string(line), coop::chat_feed::Keep::Transient);
}

}  // namespace

void Install(net::Session* session) { g_session.store(session); }

void Tick() {
    auto* s = g_session.load();
    if (!s || !s->connected() || s->role() != net::Role::Host) return;
    const uint64_t now = Now();
    if (now < g_next) return;
    g_next = now + kEveryMs;
    net::WorldDigestPayload p{};
    p.op = kOpDigest;
    if (!++g_seq) ++g_seq;
    p.sequence = g_seq;
    p.keyedProps = LocalProps();
    s->SendReliable(net::ReliableKind::WorldDigest, &p, sizeof(p));
}

void Receive(const net::WorldDigestPayload& p, uint8_t sender) {
    auto* s = g_session.load();
    if (!s || !s->connected() || sender >= net::kMaxPeers) return;
    if (s->role() == net::Role::Client) {
        if (sender != 0 || p.op != kOpDigest || !ClientSettled()) return;
        net::WorldDigestPayload a{};
        a.op = kOpAnswer;
        a.sequence = p.sequence;
        a.keyedProps = LocalProps();
        s->SendReliable(net::ReliableKind::WorldDigest, &a, sizeof(a));
        Judge(0, p.keyedProps, a.keyedProps, false);
        return;
    }
    // HOST: a client's answer to the digest it last sent; an answer to an older one is late and passed by.
    if (sender == 0 || p.op != kOpAnswer || p.sequence != g_seq) return;
    Judge(sender, LocalProps(), p.keyedProps, true);
}

void OnPeerLeft(int slot) {
    if (slot > 0 && slot < net::kMaxPeers) g_runs[slot] = Run{};
}

void OnDisconnect() {
    g_session.store(nullptr);
    for (Run& r : g_runs) r = Run{};
    g_next = 0;
}

}  // namespace coop::desync_watch
