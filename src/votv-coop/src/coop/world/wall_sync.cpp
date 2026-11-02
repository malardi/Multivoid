// coop/world/wall_sync.cpp -- see coop/world/wall_sync.h.

#include "coop/world/wall_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"

#include "ue_wrap/actors/wall.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/keyed_objects.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace coop::wall_sync {
namespace {

namespace Wl = ue_wrap::wall;
namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace W  = coop::save_record_wire;
using Record = ue_wrap::save_record::SaveRecord;
using ue_wrap::CachedObjRef;

// A wall's birth, at the native seam every spawn route finishes through: the placement factories are told apart by
// the calling frame (ue_wrap::wall::IsPlacementCaller).
UE_FACT_FN(kRowFinish, "wall_sync", L"GameplayStatics", L"FinishSpawningActor", Declared);
// The bodies that spend material, make or break a wall, or dry one: the host's.
UE_FACT_FN(kRowOverlap, "wall_sync", L"customWall_unfinished_C",
           L"BndEvt__customWall_unfinished_overlap_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowBuild, "wall_sync", L"customWall_unfinished_C", L"buildWall", Declared);
UE_FACT_FN(kRowDamage, "wall_sync", L"customWall_C", L"damage", Declared);
UE_FACT_FN(kRowFix, "wall_sync", L"customWall_C", L"fix", Declared);
UE_FACT_FN(kRowWetTick, "wall_sync", L"customWall_wetConcrete_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowDestroyed, "wall_sync", L"customWall_C", L"ReceiveDestroyed", Declared);

constexpr int kTag = 0x57414C4C;  // 'WALL'

enum Op : uint8_t { kCreate, kRemove, kRefresh, kState, kRetired, kDamage, kBuild };

constexpr size_t   kMaxRows = 1024;
constexpr uint64_t kRetryMs = 1000;
constexpr uint64_t kScanMs = 2000;
constexpr uint64_t kWetPollMs = 2000;
constexpr float    kReachUU = 1000.0f;

struct Row {
    CachedObjRef ref;
    Record record;
    bool local = false;     // placed on this machine; its record not yet captured
    bool ready = false;     // the record holds the actor's state
    bool accepted = false;  // the host's (on the host: always, once ready)
    bool incoming = false;  // a record to apply or spawn
    bool dead = false;      // retired: its actor is destroyed wherever it shows up
    bool remove = false;    // a client's own unaccepted placement went away: tell the host
    bool dirty = false;     // host: the record (or the retirement) is owed to the clients
    uint8_t  owner = 0;     // host: the slot whose placement this is
    uint16_t playerNo = 0;
    uint64_t next = 0, pollAt = 0;
};

std::atomic<coop::net::Session*> g_session{nullptr};
std::map<std::wstring, Row> g_rows;
coop::blob_chunks::Assembler g_assembler;
uint32_t g_world = 0, g_nonce = 0, g_keySeq = 0, g_blobSeq = 0;
uint64_t g_epoch = 0, g_tickAt = 0, g_scanAt = 0, g_sweepAt = 0;
int g_applying = 0;
bool g_busy = false;

enum class Reg : uint8_t { Pending, Registered, Refused };
struct Watch {
    const ue_wrap::facts::Function* row;
    SG::PreFn pre;
    SG::PostFn post;
};
SG::Verdict BeforeOverlap(const SG::Call& c);
SG::Verdict BeforeBuild(const SG::Call& c);
SG::Verdict BeforeDamage(const SG::Call& c);
SG::Verdict BeforeFix(const SG::Call& c);
SG::Verdict BeforeMirrorOnly(const SG::Call& c);
void Changed(const SG::Call& c);
const Watch kWatches[] = {
    {&kRowOverlap, &BeforeOverlap, &Changed}, {&kRowBuild, &BeforeBuild, nullptr},
    {&kRowDamage, &BeforeDamage, &Changed},   {&kRowFix, &BeforeFix, &Changed},
    {&kRowWetTick, &BeforeMirrorOnly, nullptr}, {&kRowDestroyed, &BeforeMirrorOnly, nullptr},
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
Reg  g_reg[kWatchCount] = {};
bool g_finishHooked = false, g_settled = false;

// The host's deferred material overlaps: fed from Tick, once the outline and the material have finished being made.
struct Feed {
    CachedObjRef wall, material;
    uint64_t until = 0, next = 0;
};
std::deque<Feed> g_feeds;
void* g_feeding = nullptr;
void* g_feedMaterial = nullptr;

struct Applying {
    Applying() { ++g_applying; }
    ~Applying() { --g_applying; }
};

uint64_t Now() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
coop::net::Session* Sess() { return g_session.load(std::memory_order_acquire); }
bool Host() {
    auto* s = Sess();
    return s && s->role() == coop::net::Role::Host;
}
// The session runs and has (or may get) peers: the host keeps its census from the start, so a joiner gets it whole.
bool Tracking() {
    auto* s = Sess();
    return s && s->running() && (Host() || s->connected());
}
// Someone is on the other end: a client's copies are refused, the host's overlaps deferred.
bool Connected() {
    auto* s = Sess();
    return s && s->connected();
}
bool IsClient() { return Connected() && !Host(); }

void ResetWorld() {
    g_rows.clear();
    g_assembler.Clear();
    g_feeds.clear();
    g_tickAt = g_scanAt = g_sweepAt = 0;
    ++g_epoch;
    g_world = ue_wrap::world_identity::Generation();
}
void World() {
    if (g_world != ue_wrap::world_identity::Generation()) ResetWorld();
}

// A key this lane minted: "mvpl_" + 8 hex + "_" + 8 hex (the fork's form, so a save that crossed both keeps its keys).
bool Ours(const std::wstring& key) {
    if (key.size() != 22 || key.compare(0, 5, L"mvpl_") || key[13] != L'_') return false;
    for (size_t i = 5; i < key.size(); ++i) {
        if (i == 13) continue;
        if (!((key[i] >= L'0' && key[i] <= L'9') || (key[i] >= L'a' && key[i] <= L'f'))) return false;
    }
    return true;
}
std::wstring Mint() {
    if (!g_nonce) {
        std::random_device rd;
        g_nonce = static_cast<uint32_t>(rd()) | 1u;
    }
    wchar_t key[32];
    swprintf(key, 32, L"mvpl_%08x_%08x", g_nonce, ++g_keySeq);
    return key;
}
bool ValidKey(const std::wstring& key) {
    if (key.empty() || key == L"None" || key.size() > 128) return false;
    for (auto c : key)
        if (c < 32 || c > 126) return false;
    return true;
}

bool Send(Op op, const Record& record, float amount = 0.f) {
    auto* s = Sess();
    if (!s) return false;
    std::vector<uint8_t> bytes;
    W::AppU8(bytes, op);
    if (op == kCreate || op == kState) W::SerSave(bytes, record);
    else W::AppWStr(bytes, record.key);
    if (op == kDamage) W::AppF32(bytes, amount);
    return coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::WallSave, ++g_blobSeq, bytes);
}

Row* RowOf(void* actor) {
    for (auto& [key, row] : g_rows)
        if (row.ref.Is(actor)) return &row;
    return nullptr;
}

// Host: take an existing wall into the census under the key it holds (a save's wall keeps its native key, so a
// joiner's own copy from the same save is the one the record paints).
void Enroll(void* actor) {
    if (RowOf(actor) || g_rows.size() >= kMaxRows) return;
    const std::wstring key = Wl::Key(actor);
    if (!ValidKey(key)) return;  // the native key is not set yet: a later census takes it
    auto it = g_rows.find(key);
    if (it != g_rows.end() && (it->second.dead || it->second.ref.Get())) return;
    const auto epoch = g_epoch;
    CachedObjRef ref;
    ref.Set(actor);
    Record raw, record;
    {
        Applying applying;
        if (!Wl::Capture(actor, raw) || epoch != g_epoch || !ref.Get() || !Wl::Normalize(raw, record)) return;
    }
    Row row;
    row.ref = ref;
    row.record = std::move(record);
    row.ready = row.accepted = row.dirty = true;
    g_rows.insert_or_assign(key, std::move(row));
}

void Census() {
    for (const wchar_t* name : Wl::kClasses) {
        void* cls = ue_wrap::object_index::ClassByName(name);
        if (!cls) continue;
        std::vector<CachedObjRef> found;
        ue_wrap::object_index::ForEachInstance(
            cls,
            [](void* ctx, void* a, int32_t) {
                auto& out = *static_cast<std::vector<CachedObjRef>*>(ctx);
                if (!a || R::NameStartsWith(R::NameOf(a), L"Default__") || out.size() >= 256) return;
                if (ue_wrap::world_identity::WorldOf(a) != ue_wrap::world_identity::CurrentWorld()) return;
                CachedObjRef r;
                r.Set(a);
                out.push_back(r);
            },
            &found);
        const auto epoch = g_epoch;
        for (const auto& r : found) {
            if (void* a = r.Get()) Enroll(a);
            if (epoch != g_epoch) return;
        }
    }
}

// ---- the seams --------------------------------------------------------------------------------
// A wall born from a placement factory on this machine takes a minted key; its record is captured next tick.
void Finished(void*, void*, void* result) {
    if (!Tracking() || g_applying || !result) return;
    const Wl::Kind kind = Wl::KindOf(result);
    if (kind == Wl::Kind::None ||
        !Wl::IsPlacementCaller(kind, ue_wrap::ufunction_hook::CurrentCallerFrame().function))
        return;
    World();
    if (g_rows.size() >= kMaxRows) {
        UE_LOGW("[WALL] the wall table is full; a new wall is not shared");
        return;
    }
    const auto key = Mint();
    const auto epoch = g_epoch;
    CachedObjRef ref;
    ref.Set(result);
    {
        Applying applying;
        if (!Wl::SetKey(result, key) || !ref.Get() || epoch != g_epoch) return;
    }
    Row row;
    row.ref = ref;
    row.local = true;
    row.record.key = key;
    g_rows.emplace(key, std::move(row));
    UE_LOGI("[WALL] %s placed %ls key='%ls'", Host() ? "host" : "client", R::ClassNameOf(result).c_str(), key.c_str());
}

// The host feeds an outline from Tick (the overlap can fire before the outline's size and material are final, and
// a client's copy never feeds); with no one connected the host's own overlap runs as it is.
SG::Verdict BeforeOverlap(const SG::Call& c) {
    World();
    if (!Connected()) return SG::Verdict::Run;
    if (!Host()) return SG::Verdict::Cancel;
    if (c.fromOurCode && g_feeding == c.object) return SG::Verdict::Run;
    static R::ParamOffset other(L"OtherActor");
    const int32_t off = c.function ? other.Of(c.function) : -1;
    const int32_t size = c.function ? R::FunctionFrameSize(c.function) : 0;
    if (!c.locals || off < 0 || off > size || size - off < static_cast<int32_t>(sizeof(void*))) return SG::Verdict::Cancel;
    void* material = nullptr;
    std::memcpy(&material, c.locals + off, sizeof(material));
    if (!material || !R::IsLive(material)) return SG::Verdict::Cancel;
    if (g_feeding == c.object && g_feedMaterial == material) return SG::Verdict::Cancel;
    for (const auto& f : g_feeds)
        if (f.wall.Is(c.object) && f.material.Is(material)) return SG::Verdict::Cancel;
    if (g_feeds.size() < 256) {
        Feed f;
        f.wall.Set(c.object);
        f.material.Set(material);
        f.until = Now() + 3000;
        g_feeds.push_back(f);
    }
    if (void* a = c.object) Enroll(a);
    return SG::Verdict::Cancel;
}

// buildWall makes the wall from a full outline: the host's. A client's creative build (the wall builder's addWall
// with isCheat) goes to the host as a request.
SG::Verdict BeforeBuild(const SG::Call& c) {
    World();
    if (!IsClient()) return SG::Verdict::Run;
    if (c.fromOurCode || g_applying) return SG::Verdict::Cancel;
    if (c.callerObject && c.callerObject == coop::hand_item::LocalHandActor() &&
        Wl::IsBuilderCall(c.callerObject, c.callerFunction) && c.function && c.locals) {
        int32_t off = -1;
        uint8_t mask = 0;
        const int32_t size = R::FunctionFrameSize(c.function);
        if (R::FindBoolProperty(c.function, L"isCheat", off, mask) && off >= 0 && off < size && mask &&
            (c.locals[off] & mask)) {
            Record r;
            r.key = Wl::Key(c.object);
            if (ValidKey(r.key) && Send(kBuild, r))
                UE_LOGI("[WALL] client: creative build of key='%ls' sent", r.key.c_str());
        }
    }
    return SG::Verdict::Cancel;
}

// A client's hit on a wall is dealt by the host: the amount crosses, the client's own copy takes nothing.
SG::Verdict BeforeDamage(const SG::Call& c) {
    World();
    if (!IsClient()) {
        if (Host() && Tracking() && c.object) Enroll(c.object);
        return SG::Verdict::Run;
    }
    if (c.fromOurCode || g_applying) return SG::Verdict::Cancel;
    static R::ParamOffset damageOff(L"damage");
    const int32_t off = c.function ? damageOff.Of(c.function) : -1;
    float amount = 0.f;
    if (c.locals && off >= 0) std::memcpy(&amount, c.locals + off, sizeof(amount));
    Record r;
    r.key = Wl::Key(c.object);
    if (std::isfinite(amount) && amount > 0.f && ValidKey(r.key)) Send(kDamage, r, (std::min)(amount, 1000.f));
    return SG::Verdict::Cancel;
}

// The trowel's repair spends cement when fix answers isFine=false. A client's copy is refused and answers isFine,
// so nothing is spent for a repair that did not happen.
SG::Verdict BeforeFix(const SG::Call& c) {
    World();
    if (!IsClient()) {
        if (Host() && Tracking() && c.object) Enroll(c.object);
        return SG::Verdict::Run;
    }
    int32_t off = -1;
    uint8_t mask = 0;
    const int32_t size = c.function ? R::FunctionFrameSize(c.function) : 0;
    const int32_t param = c.function ? R::FindParamOffset(c.function, L"isFine") : -1;
    if (c.locals && param >= 0 && R::FindBoolProperty(c.function, L"isFine", off, mask) && mask && off >= param &&
        off < size) {
        c.locals[off] |= mask;
        if (auto* out = SG::OutParamPtr(c, param)) out[off - param] |= mask;
    }
    return SG::Verdict::Cancel;
}

// A wet wall's drying and a wall's break debris: the host's, so a client's copy runs neither.
SG::Verdict BeforeMirrorOnly(const SG::Call&) { return IsClient() ? SG::Verdict::Cancel : SG::Verdict::Run; }

void Changed(const SG::Call& c) {
    if (!Host() || !Tracking() || g_applying) return;
    World();
    if (Row* row = RowOf(c.object)) {
        row->dirty = true;
        row->next = 0;
    }
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (SG::WatchName(*kWatches[i].row, kTag, kWatches[i].pre, kWatches[i].post)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[WALL] the script-body gate refused the watch on '%ls' of '%ls'", kWatches[i].row->Name(),
                kWatches[i].row->Class());
    }
    if (!g_finishHooked) {
        const auto fn = ue_wrap::facts::Resolve(kRowFinish);
        if (fn && ue_wrap::ufunction_hook::InstallPostHook(fn, &Finished)) g_finishHooked = true;
    }
}

void Settle() {
    if (g_settled || SG::PendingNameCount() != 0) return;
    for (int i = 0; i < kWatchCount; ++i)
        if (g_reg[i] == Reg::Pending || (g_reg[i] == Reg::Registered && !SG::NameWatchSettled(*kWatches[i].row, kTag)))
            return;
    g_settled = true;
    int live = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Registered && SG::NameWatchLive(*kWatches[i].row, kTag)) ++live;
        else
            UE_LOGW("[WALL] the watch on '%ls' of '%ls' is dead -- that body runs on every copy",
                    kWatches[i].row->Name(), kWatches[i].row->Class());
    }
    UE_LOGI("[WALL] %d of %d wall watches are live; the placement seam is %s", live, kWatchCount,
            g_finishHooked ? "hooked" : "NOT hooked");
}

// ---- the host's material feed -------------------------------------------------------------------
void DrainFeeds() {
    const auto epoch = g_epoch;
    const auto now = Now();
    const size_t count = g_feeds.size();
    for (size_t i = 0, work = 0; i < count && work < 8 && !g_feeds.empty(); ++i) {
        Feed f = g_feeds.front();
        g_feeds.pop_front();
        void* wall = f.wall.Get();
        void* material = f.material.Get();
        if (!wall || !material || now > f.until) continue;
        if (now < f.next) {
            g_feeds.push_back(f);
            continue;
        }
        ++work;
        g_feeding = wall;
        g_feedMaterial = material;
        const bool handled = Wl::Feed(wall, material, [&] { return epoch == g_epoch && Host() && f.wall.Get(); });
        g_feeding = g_feedMaterial = nullptr;
        if (epoch != g_epoch) return;
        if (Row* row = f.wall.Get() ? RowOf(f.wall.Get()) : nullptr) {
            row->dirty = true;
            row->next = 0;
        }
        if (!handled && f.wall.Get() && f.material.Get() && g_feeds.size() < 256) {
            f.next = now + 250;
            g_feeds.push_back(f);
        }
    }
}

// ---- the wire -------------------------------------------------------------------------------------
bool Reach(uint8_t slot, const Record& r) {
    auto* s = Sess();
    if (!s) return false;
    const ue_wrap::FVector p{r.xform[4], r.xform[5], r.xform[6]};
    return static_cast<bool>(coop::element::IntentTarget::ForClientIntent(*s, slot, kReachUU).AuthorizeSegment(p, p));
}

void Dead(Row& row) {
    row.dead = true;
    row.remove = row.incoming = false;
    row.accepted = true;
    row.dirty = Host();
    row.next = 0;
}

void HostReceive(uint8_t op, Record& record, float amount, uint8_t slot) {
    auto* s = Sess();
    auto it = g_rows.find(record.key);
    if (op == kRefresh) {
        if (it != g_rows.end() && it->second.accepted) {
            it->second.dirty = true;
            it->second.next = 0;
        }
        return;
    }
    if (op == kDamage || op == kBuild) {
        void* a = it != g_rows.end() ? it->second.ref.Get() : ue_wrap::keyed_objects::Resolve(record.key.c_str());
        if (!a || Wl::KindOf(a) != (op == kDamage ? Wl::Kind::Wall : Wl::Kind::Outline)) return;
        if (!coop::element::IntentTarget::ForClientIntent(*s, slot, 600.f).Authorize(a)) return;
        Enroll(a);
        if (op == kDamage) {
            const bool ok = Wl::Damage(a, amount);
            UE_LOGI("[WALL] host dealt slot %u's %.1f to key='%ls' -> %s", static_cast<unsigned>(slot), amount,
                    record.key.c_str(), ok ? "ok" : "not taken");
        } else {
            bool allowed = false;
            if (!Wl::CreativeAllowed(a, allowed) || !allowed) return;
            const bool ok = Wl::FinishCreative(a);
            UE_LOGI("[WALL] host built slot %u's outline key='%ls' (creative) -> %s", static_cast<unsigned>(slot),
                    record.key.c_str(), ok ? "ok" : "not taken");
        }
        return;
    }
    if (op == kCreate && !Ours(record.key)) return;  // a client never names a wall the save already keys
    if (it != g_rows.end()) {
        if (op == kCreate) {
            if (it->second.accepted) it->second.dirty = true;
            return;
        }
        // kRemove: the placer cancels its own outline, or a peer within reach removes a wall the host still has.
        Row& row = it->second;
        if (row.dead) {
            row.dirty = true;
            return;
        }
        void* a = row.ref.Get();
        if (a && !coop::element::IntentTarget::ForClientIntent(*s, slot, kReachUU).Authorize(a) && !Reach(slot, row.record))
            return;
        if (!a && row.owner != slot) return;
        const auto epoch = g_epoch;
        {
            Applying applying;
            if (a) ue_wrap::engine::DestroyActor(a);
        }
        if (epoch != g_epoch || row.ref.Get()) return;
        it = g_rows.find(record.key);
        if (it != g_rows.end()) Dead(it->second);
        return;
    }
    if (g_rows.size() >= kMaxRows || !coop::roster_ledger::Get(slot).playerNo) return;
    Row row;
    row.record = record;
    row.owner = slot;
    row.playerNo = coop::roster_ledger::Get(slot).playerNo;
    if (op == kRemove) {
        // A cancel that overtook its creation: the tombstone keeps the late creation from raising it.
        if (!ue_wrap::keyed_objects::Available() || ue_wrap::keyed_objects::Resolve(record.key.c_str())) return;
        Dead(row);
    } else {
        if (!Reach(slot, record)) {
            UE_LOGI("[WALL] host: slot %u's placement key='%ls' is out of its reach -- refused",
                    static_cast<unsigned>(slot), record.key.c_str());
            return;
        }
        row.ready = row.incoming = true;
    }
    g_rows.emplace(record.key, std::move(row));
}

void ClientReceive(uint8_t op, Record& record) {
    auto it = g_rows.find(record.key);
    if (it == g_rows.end()) {
        if (g_rows.size() >= kMaxRows) return;
        Row row;
        row.record = record;
        row.ready = row.accepted = true;
        row.incoming = op == kState;
        if (op == kRetired) row.dead = true;
        g_rows.emplace(record.key, std::move(row));
        return;
    }
    Row& row = it->second;
    if (op == kRetired) {
        const CachedObjRef ref = row.ref;
        Dead(row);
        Applying applying;
        if (void* a = ref.Get()) ue_wrap::engine::DestroyActor(a);
        return;
    }
    if (row.dead) return;
    row.accepted = true;
    row.local = false;
    row.record = record;
    row.incoming = true;
    row.next = 0;
}

// ---- the tick -------------------------------------------------------------------------------------
void TickRows(uint64_t now) {
    const auto epoch = g_epoch;
    std::vector<std::wstring> keys;
    keys.reserve(g_rows.size());
    for (const auto& [key, row] : g_rows) keys.push_back(key);
    int work = 0;
    for (const auto& key : keys) {
        if (epoch != g_epoch || work > 4) return;
        auto it = g_rows.find(key);
        if (it == g_rows.end() || now < it->second.next) continue;
        Row& row = it->second;
        void* a = row.ref.Get();
        // A retired wall: its actor goes wherever it is still found.
        if (row.dead) {
            if (!a && !Host() && ue_wrap::keyed_objects::Available()) {
                a = ue_wrap::keyed_objects::Resolve(key.c_str());
                if (a && Wl::KindOf(a) == Wl::Kind::None) a = nullptr;
                if (!a) row.next = now + 2000;  // a tombstone with nothing to destroy is asked again slowly
            }
            if (a) {
                ++work;
                row.next = now + 500;
                Applying applying;
                ue_wrap::engine::DestroyActor(a);
                if (epoch != g_epoch) return;
                continue;
            }
            if (Host() && row.dirty && Connected()) {
                ++work;
                row.next = now + kRetryMs;
                if (Send(kRetired, row.record) && epoch == g_epoch) {
                    it = g_rows.find(key);
                    if (it != g_rows.end()) it->second.dirty = false;
                }
            }
            continue;
        }
        // A placement of this machine: its record, once the actor holds it.
        if (a && row.local && !row.ready) {
            ++work;
            row.next = now + 250;
            Record raw, normalized;
            {
                Applying applying;
                if (!Wl::Capture(a, raw) || epoch != g_epoch || !Wl::Normalize(raw, normalized) ||
                    normalized.key != key)
                    continue;
            }
            it = g_rows.find(key);
            if (it == g_rows.end()) continue;
            it->second.record = std::move(normalized);
            it->second.ready = true;
            it->second.accepted = Host();
            it->second.dirty = Host();
            it->second.next = 0;
            continue;
        }
        // A record to make true here: the actor its key names, or a new one.
        if (row.incoming) {
            ++work;
            row.next = now + 500;
            if (Host() && coop::roster_ledger::Get(row.owner).playerNo != row.playerNo) {
                g_rows.erase(it);
                continue;
            }
            if (!ue_wrap::keyed_objects::Available()) continue;
            Record record = row.record;
            void* target = a ? a : ue_wrap::keyed_objects::Resolve(key.c_str());
            if (epoch != g_epoch) return;
            {
                Applying applying;
                if (target) {
                    if (R::ClassNameOf(target) != record.className) {
                        // The key names another kind of actor here (a wet wall dried locally into a wall): that copy
                        // goes, and the record's actor is made.
                        if (Wl::KindOf(target) == Wl::Kind::None) continue;
                        ue_wrap::engine::DestroyActor(target);
                        target = nullptr;
                    } else if (Host()) {
                        Record raw, normalized;
                        if (!Wl::Capture(target, raw) || !Wl::Normalize(raw, normalized)) continue;
                        record = std::move(normalized);  // an actor the host has wins a colliding placement
                    } else if (!Wl::Apply(target, record)) {
                        continue;
                    }
                }
                if (!target) target = Wl::Spawn(record);
            }
            if (epoch != g_epoch) return;
            if (!target) continue;
            it = g_rows.find(key);
            if (it == g_rows.end()) continue;
            it->second.record = std::move(record);
            it->second.ref.Set(target);
            it->second.incoming = false;
            it->second.accepted = true;
            it->second.dirty = Host();
            it->second.next = 0;
            continue;
        }
        // Its actor went away.
        if (!a && row.ref.Raw()) {
            if (Host()) {
                if (row.ready) Dead(row);
                else g_rows.erase(it);
                continue;
            }
            row.ref.Reset();
            if (!row.accepted) {
                // A client's own placement it cancelled before the host took it.
                row.remove = true;
            } else {
                // The host's wall is gone only here: ask for it again rather than remove it for everyone.
                row.local = false;
                row.next = now + kRetryMs;
                Send(kRefresh, row.record);
                if (epoch != g_epoch) return;
                continue;
            }
        }
        // The host: a wet wall's drying is polled; the rest move at their verbs.
        if (Host() && a && row.accepted && Wl::IsWet(a) && now >= row.pollAt) {
            row.pollAt = now + kWetPollMs;
            row.dirty = true;
        }
        if (!Connected()) continue;
        if (Host() && row.dirty && a) {
            ++work;
            row.next = now + kRetryMs;
            Record raw, normalized;
            {
                Applying applying;
                if (!Wl::Capture(a, raw) || epoch != g_epoch || !Wl::Normalize(raw, normalized) || normalized.key != key)
                    continue;
            }
            it = g_rows.find(key);
            if (it == g_rows.end()) continue;
            it->second.record = std::move(normalized);
            if (Send(kState, it->second.record) && epoch == g_epoch) {
                it = g_rows.find(key);
                if (it != g_rows.end()) it->second.dirty = false;
            }
            continue;
        }
        if (!Host() && (row.remove || (row.ready && !row.accepted))) {
            ++work;
            row.next = now + kRetryMs;
            const Op op = row.remove ? kRemove : kCreate;
            const bool sent = Send(op, row.record);
            if (epoch != g_epoch) return;
            if (op == kRemove && sent) g_rows.erase(key);
            else if (op == kCreate && sent) UE_LOGI("[WALL] client: placement key='%ls' sent", key.c_str());
        }
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (Sess() != session) {
        g_session.store(session, std::memory_order_release);
        ResetWorld();
    }
    RegisterWatches();
}

void Tick() {
    if (g_busy) return;
    g_busy = true;
    struct Busy {
        ~Busy() { g_busy = false; }
    } busy;
    SG::ResolvePendingNames();
    Settle();
    if (!Tracking()) {
        g_feeds.clear();
        return;
    }
    World();
    const auto now = Now();
    if (now < g_tickAt) return;
    g_tickAt = now + 100;
    const auto epoch = g_epoch;
    if (Host()) {
        DrainFeeds();
        if (epoch != g_epoch) return;
        if (now >= g_scanAt) {
            g_scanAt = now + kScanMs;
            Census();
            if (epoch != g_epoch) return;
        }
    }
    if (now >= g_sweepAt) {
        g_sweepAt = now + 1000;
        g_assembler.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(20));
    }
    TickRows(now);
}

void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t slot) {
    if (!Connected() || slot >= coop::net::kMaxPeers || (Host() ? slot == 0 : slot != 0)) return;
    World();
    std::vector<uint8_t> bytes;
    if (!g_assembler.OnChunk(chunk, slot, bytes) || bytes.size() > 64 * 1024) return;
    size_t offset = 0;
    uint8_t op = 0;
    if (!W::RdU8(bytes, offset, op) || op > kBuild) return;
    if (Host() ? (op == kState || op == kRetired) : (op != kState && op != kRetired)) return;
    Record record;
    float amount = 0.f;
    if (op == kCreate || op == kState) {
        Record input;
        if (!W::DeSave(bytes, offset, input) || !Wl::Normalize(input, record)) return;
    } else if (!W::RdWStr(bytes, offset, record.key)) {
        return;
    }
    if (op == kDamage && (!W::RdF32(bytes, offset, amount) || !std::isfinite(amount) || amount <= 0.f || amount > 1000.f))
        return;
    if (offset != bytes.size() || !ValidKey(record.key)) return;
    if (Host()) HostReceive(op, record, amount, slot);
    else ClientReceive(op, record);
}

void QueueConnect(int slot) {
    if (!Host() || slot <= 0 || slot >= coop::net::kMaxPeers) return;
    World();
    for (auto& [key, row] : g_rows)
        if (row.accepted) {
            row.dirty = true;
            row.next = 0;
        }
}

void OnPeerLeft(int slot) {
    if (slot < 0 || slot >= coop::net::kMaxPeers) return;
    g_assembler.ClearSlot(static_cast<uint8_t>(slot));
    // A placement the host had not made yet goes with its placer; a made one is the host's world.
    std::erase_if(g_rows, [slot](const auto& p) {
        return Host() && p.second.incoming && p.second.owner == slot && !p.second.ref.Get();
    });
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    ResetWorld();
}

}  // namespace coop::wall_sync
