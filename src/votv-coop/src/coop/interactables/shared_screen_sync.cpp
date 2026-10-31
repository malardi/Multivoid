// coop/interactables/shared_screen_sync.cpp -- see coop/interactables/shared_screen_sync.h.
//
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/coop/interactables/shared_screen_sync.cpp (MIT).
// Differences from the fork: the host is the operator while nobody holds the laptop (the fork pinned every
// screen to the last operator's), a text-only change is paced, an observer repaints on a change or once a
// second rather than every poll, the canonical is not echoed to the operator whose report it is, and the final
// screen is flushed through device_occupancy's release hold.

#include "coop/interactables/shared_screen_sync.h"

#include "coop/interactables/device_occupancy.h"
#include "coop/interactables/shared_screen_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/shared_screen.h"
#include "ue_wrap/engine/world_identity.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace coop::shared_screen_sync {
namespace {

namespace UI = ue_wrap::shared_screen;
namespace W  = coop::shared_screen_wire;
namespace DO = coop::device_occupancy;
using Clock = std::chrono::steady_clock;

const wchar_t* const kClaim = L"laptop";
constexpr uint8_t kNobody = 0xFF;

constexpr auto kTick        = std::chrono::milliseconds(250);   // the capture and apply cadence
constexpr auto kShapeGap    = std::chrono::milliseconds(500);   // a page, visibility or scroll change, at most this often
constexpr auto kTextGap     = std::chrono::milliseconds(1000);  // a text-only change, at most this often
constexpr auto kRefresh     = std::chrono::milliseconds(1000);  // an observer repaints unchanged rows this often
constexpr int  kOperatorRepaints = 3;   // refused inherited repaints on a readable UI before the operator resumes
constexpr int  kMaxReleaseHolds  = 8;   // ticks a release waits for the final screen to be accepted

std::atomic<coop::net::Session*> g_session{nullptr};
coop::blob_chunks::Assembler g_asm;

std::vector<uint8_t> g_canonical;     // HOST: the rows every peer shows, encoded
std::vector<uint8_t> g_sent;          // CLIENT: the rows this operator last reported, encoded
std::string          g_sentShape;     // the shape (everything but the texts) of the last capture sent
std::vector<UI::Row> g_target;        // what this peer shows while it is not the operator
bool g_targetFresh = false;           // the target changed since this peer last applied it

std::array<bool, coop::net::kMaxPeers>     g_owed{};   // HOST: the canonical is owed to this slot
std::array<uint32_t, coop::net::kMaxPeers> g_seen{};   // HOST: the newest report revision taken from this slot

uint32_t g_sequence = 0;   // blob sequence, this sender's
uint32_t g_lease = 0;      // the operator's tenure (host-minted)
uint32_t g_revision = 0;   // HOST: the canonical's order; CLIENT: the newest canonical taken
uint32_t g_command = 0;    // CLIENT: its reports' order
uint32_t g_world = 0;
uint8_t  g_holder = kNobody;
bool     g_apply = false;  // inherit the target before authoring
int      g_failures = 0;
int      g_releaseHolds = 0;
bool     g_busy = false;
bool     g_holdRegistered = false;
Clock::time_point g_next{}, g_lastSend{}, g_nextRefresh{}, g_nextBroadcast{};

bool Active(coop::net::Session* s) { return s && s->running() && s->connected(); }
bool IsHost(coop::net::Session* s) { return s && s->role() == coop::net::Role::Host; }
uint32_t Next(uint32_t& v) { if (!++v) ++v; return v; }
bool Newer(uint32_t a, uint32_t b) { return a && (!b || static_cast<int32_t>(a - b) > 0); }

// Every member but the texts, so a page, a visibility or a scroll is told from a text that ticked.
std::string ShapeOf(const std::vector<UI::Row>& rows) {
    std::string shape;
    for (const UI::Row& r : rows) {
        shape += r.path;
        shape += ':';
        shape += std::to_string(r.visibility) + ',' + std::to_string(r.flags) + ',' + std::to_string(r.tab) + ',' +
                 std::to_string(static_cast<int32_t>(r.scroll));
        shape += ';';
    }
    return shape;
}

void SetTarget(std::vector<UI::Row> rows) {
    g_target = std::move(rows);
    g_targetFresh = true;
}

// The world (or the session) changed under this lane: everything it holds named the old one. The lease and the
// revisions stay monotonic, so a packet of the old world never reads as the new one's.
void Clear(coop::net::Session* s) {
    g_asm.Clear();
    g_canonical.clear();
    g_sent.clear();
    g_sentShape.clear();
    g_target.clear();
    g_targetFresh = false;
    g_owed.fill(false);
    g_seen.fill(0);
    g_holder = kNobody;
    g_apply = false;
    g_failures = 0;
    g_releaseHolds = 0;
    g_next = g_lastSend = g_nextRefresh = g_nextBroadcast = Clock::time_point{};
    g_world = ue_wrap::world_identity::Generation();
    UI::ResetWritten();
    if (IsHost(s)) {
        Next(g_lease);
        Next(g_revision);
    }
}

void World(coop::net::Session* s) {
    if (g_world != ue_wrap::world_identity::Generation()) Clear(s);
}

// HOST: the canonical changed; owed to every ready client but `exceptSlot`.
void Dirty(coop::net::Session* s, int exceptSlot = -1) {
    Next(g_revision);
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot)
        if (slot != exceptSlot && s->HasPeerConn(slot) && s->IsSlotWorldReady(slot)) g_owed[slot] = true;
}

// HOST: a new holder is a new lease, and whoever operates under it first inherits the screen as it stands.
void Lease(coop::net::Session* s) {
    if (!IsHost(s)) return;
    const uint8_t holder = DO::HolderOf(kClaim);
    if (g_lease && holder == g_holder) return;
    g_holder = holder;
    Next(g_lease);
    g_seen.fill(0);
    g_apply = !g_target.empty();
    g_failures = 0;
    Dirty(s);
}

// Whether this peer's screen is the one everyone sees: it holds the laptop under the current lease, or it is the
// host and nobody holds it.
bool Author(coop::net::Session* s) {
    const uint8_t me = coop::players::Registry::Get().LocalPeerId();
    if (IsHost(s) && g_holder == kNobody) return true;
    return g_lease && g_holder == me && DO::LocalHolds(kClaim);
}

// Whether a capture of this shape may go out now: a text-only change every kTextGap, any other every kShapeGap.
bool Paced(const std::string& shape, Clock::time_point now) {
    return now < g_lastSend + (shape == g_sentShape ? kTextGap : kShapeGap);
}

// The operator's screen, read and published: the host's becomes the canonical, a client's is reported, each at
// the pace above unless `final`. True when nothing is left to send.
bool Capture(coop::net::Session* s, bool final) {
    std::vector<UI::Row> rows;
    if (!UI::Read(rows)) return true;   // no desktop to show: nothing to flush either
    std::vector<uint8_t> bytes = W::Encode(rows);
    if (bytes.empty()) return true;     // past what the wire carries: this screen stays local
    const auto now = Clock::now();
    if (IsHost(s)) {
        if (bytes == g_canonical) return true;
        const std::string shape = ShapeOf(rows);
        if (!final && Paced(shape, now)) return true;
        g_sentShape = shape;
        g_lastSend = now;
        g_canonical = std::move(bytes);
        SetTarget(std::move(rows));
        g_targetFresh = false;   // the host's own screen already shows it
        Dirty(s);
        return true;
    }
    if (bytes == g_sent) return true;
    const std::string shape = ShapeOf(rows);
    if (!final && Paced(shape, now)) return true;
    const std::vector<uint8_t> blob = W::EncodeSnapshot(false, g_lease, Next(g_command), g_holder, bytes);
    if (blob.empty()) return true;
    if (!coop::blob_chunks::SendBlobToSlot(s, 0, coop::net::ReliableKind::SharedScreen, Next(g_sequence), blob))
        return false;   // backpressure: the next tick reads and sends again
    g_sent = std::move(bytes);
    g_sentShape = shape;
    g_lastSend = now;
    // What this peer shows once it lets go, until the host's next canonical: its own last screen, not the one
    // it inherited.
    SetTarget(std::move(rows));
    g_targetFresh = false;
    return true;
}

// HOST: the canonical to every slot it is owed to.
void Broadcast(coop::net::Session* s, Clock::time_point now) {
    if (!IsHost(s) || g_canonical.empty() || now < g_nextBroadcast) return;
    std::vector<uint8_t> blob;
    bool any = false;
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (!g_owed[slot]) continue;
        if (!s->HasPeerConn(slot) || !s->IsSlotWorldReady(slot)) { g_owed[slot] = false; continue; }
        if (blob.empty()) blob = W::EncodeSnapshot(true, g_lease, g_revision, g_holder, g_canonical);
        if (blob.empty()) return;
        if (coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::SharedScreen, Next(g_sequence), blob)) {
            g_owed[slot] = false;
            any = true;
        }
    }
    // Chunks are 228 bytes apiece on the bulk lane: a burst is followed by a breath.
    if (any) g_nextBroadcast = now + kTick;
}

// device_occupancy's release hold: the operator lets go of the laptop only once its final screen is sent, so the
// next operator inherits what this one left. Bounded; a host operator's capture is local and never holds.
bool ReleaseHold(const wchar_t* key) {
    if (!key || std::wcscmp(key, kClaim) != 0) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!Active(s) || !ue_wrap::game_thread::IsGameThread()) return false;
    World(s);
    if (!Author(s) || g_apply) { g_releaseHolds = 0; return false; }
    if (Capture(s, /*final=*/true) || ++g_releaseHolds > kMaxReleaseHolds) {
        if (g_releaseHolds > kMaxReleaseHolds)
            UE_LOGW("shared_screen_sync: the final screen was refused %d times -- the laptop is released without it",
                    kMaxReleaseHolds);
        g_releaseHolds = 0;
        return false;
    }
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    if (g_session.load(std::memory_order_acquire) != session) {
        g_session.store(session, std::memory_order_release);
        Clear(session);
    }
    if (!g_holdRegistered) {
        DO::RegisterReleaseHold(&ReleaseHold);
        g_holdRegistered = true;
    }
}

void Tick() {
    if (!ue_wrap::game_thread::IsGameThread() || g_busy) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!Active(s)) return;
    g_busy = true;
    struct Busy { ~Busy() { g_busy = false; } } busy;
    World(s);
    Lease(s);
    const auto now = Clock::now();
    if (now < g_next) return;
    g_next = now + kTick;
    if (!g_asm.Idle()) g_asm.Sweep(now, std::chrono::seconds(5));

    const bool author = Author(s);
    if (!g_target.empty() && (g_apply || !author) && (g_apply || g_targetFresh || now >= g_nextRefresh)) {
        g_nextRefresh = now + kRefresh;
        const uint32_t lease = g_lease;
        const bool applied = UI::Apply(g_target);
        if (lease != g_lease) return;   // the apply ran Blueprint code; a new lease is the next tick's
        g_targetFresh = false;
        if (applied) {
            g_apply = false;
            g_failures = 0;
        } else if (g_apply && author) {
            std::vector<UI::Row> readable;
            if (!UI::Read(readable)) {
                g_failures = 0;   // no desktop to repaint yet: keep waiting for it
            } else if (++g_failures >= kOperatorRepaints) {
                UE_LOGW("shared_screen_sync: the inherited screen was refused %d times on a readable UI -- the "
                        "operator resumes from its own", g_failures);
                g_apply = false;
                g_failures = 0;
            }
        }
    }
    if ((!g_apply && Author(s)) || (IsHost(s) && g_canonical.empty())) Capture(s, /*final=*/false);
    Broadcast(s, now);
}

void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot) {
    if (!ue_wrap::game_thread::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!Active(s) || senderSlot >= coop::net::kMaxPeers) return;
    World(s);
    Lease(s);
    const bool host = IsHost(s);
    if (host) {
        // Only the laptop's holder reports, and only once its world is up.
        if (senderSlot == 0 || senderSlot != g_holder || !s->IsSlotWorldReady(senderSlot)) return;
    } else if (senderSlot != 0) {
        return;
    }
    constexpr size_t kChunkData = sizeof(coop::net::BlobChunkPayload{}.data);
    constexpr size_t kMaxChunks = (W::kMaxBytes + W::kEnvelopeBytes + kChunkData - 1) / kChunkData;
    if (chunk.chunks > kMaxChunks) return;
    std::vector<uint8_t> bytes;
    if (!g_asm.OnChunk(chunk, senderSlot, bytes)) return;
    W::Snapshot snap;
    if (!W::DecodeSnapshot(bytes, snap) || (snap.owner != kNobody && snap.owner >= coop::net::kMaxPeers)) {
        UE_LOGW("shared_screen_sync: a malformed snapshot from slot %u -- dropped", static_cast<unsigned>(senderSlot));
        return;
    }
    if (host) {
        if (snap.canonical || snap.owner != senderSlot || snap.lease != g_lease || !Newer(snap.revision, g_seen[senderSlot]))
            return;
        g_seen[senderSlot] = snap.revision;
        std::vector<uint8_t> rows = W::Encode(snap.rows);
        if (rows.empty() || rows == g_canonical) return;
        g_canonical = std::move(rows);
        SetTarget(std::move(snap.rows));
        Dirty(s, senderSlot);   // the operator already shows it
        return;
    }
    if (!snap.canonical || !Newer(snap.revision, g_revision)) return;
    const bool newLease = snap.lease != g_lease;
    g_lease = snap.lease;
    g_revision = snap.revision;
    g_holder = snap.owner;
    if (newLease) {
        g_sent.clear();
        g_sentShape.clear();
        g_failures = 0;
        g_apply = true;
    }
    // A canonical of this operator's own lease is its own report coming back: never rolled back over its newer edits.
    if (newLease || !Author(s)) SetTarget(std::move(snap.rows));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (peerSlot > 0 && peerSlot < static_cast<int>(coop::net::kMaxPeers)) g_owed[peerSlot] = true;
}

void OnPeerGone(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_owed[peerSlot] = false;
    g_seen[peerSlot] = 0;
    g_asm.ClearSlot(static_cast<uint8_t>(peerSlot));
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    Clear(nullptr);
    g_sequence = g_lease = g_revision = g_command = 0;
}

}  // namespace coop::shared_screen_sync
