// coop/props/open_container_sync.cpp -- see coop/props/open_container_sync.h.

#include "coop/props/open_container_sync.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_wire_parity.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::open_container_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace pf = coop::net::propspawn_flags;

// The open containers, each by its exact class (ForEachInstance takes no subclasses), from the game's
// prop_openContainer_C family. The garbage container is not here: garbage_sync owns its client brain.
const wchar_t* const kClasses[] = {
    L"prop_openContainer_C",
    L"prop_openContainer_bowl_L_C", L"prop_openContainer_bowl_M_C", L"prop_openContainer_bowl_S_C",
    L"prop_openContainer_easterbasket_C", L"prop_openContainer_ffWdL_C", L"prop_openContainer_ffWdLS_C",
    L"prop_openContainer_fridgeshelf1_C", L"prop_openContainer_fridgeshelf2_C",
    L"prop_openContainer_fridgeshelf3_C", L"prop_openContainer_plate_L_C", L"prop_openContainer_plate_S_C",
    L"prop_openContainer_scrapbox_C", L"prop_openContainer_wastebasket_C",
};
constexpr int kClassCount = static_cast<int>(sizeof(kClasses) / sizeof(kClasses[0]));

constexpr uint64_t kPeriodMs = 150;           // a carried basket's contents follow at about 7 Hz
constexpr float    kResendCm2 = 2.0f * 2.0f;   // re-send an item only once it moved more than 2 cm
constexpr int32_t  kMaxItemsPerContainer = 64; // a sane bound on a game array read raw

struct ActorArray { void** data; int32_t num; int32_t max; };

struct Sent { ue_wrap::FVector pos; uint8_t flags; };
std::unordered_map<uint32_t, Sent> g_lastSent;  // eid -> what the clients were last told

void* g_cls[kClassCount] = {};
int32_t g_itemsOff = -2;  // prop_openContainer_C.itemsInside; -2 unresolved, -1 absent
uint64_t g_nextMs = 0;
uint64_t g_sentCount = 0;
bool g_firstSaid = false;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Ctx { coop::net::Session* s; std::vector<void*>* containers; };

void Collect(void* ctx, void* obj, int32_t /*index*/) {
    static_cast<Ctx*>(ctx)->containers->push_back(obj);
}

void SendItem(coop::net::Session& s, void* item) {
    const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(item);
    if (eid == coop::element::kInvalidId || eid == 0) return;  // not a host prop the clients mirror
    ue_wrap::FVector cur{};
    if (!E::TryGetActorLocation(item, cur)) return;
    const uint8_t flags = coop::prop_wire_parity::PhysFlagsOf(item);
    const uint32_t key = static_cast<uint32_t>(eid);
    const auto it = g_lastSent.find(key);
    if (it != g_lastSent.end()) {
        const float dx = cur.X - it->second.pos.X, dy = cur.Y - it->second.pos.Y, dz = cur.Z - it->second.pos.Z;
        const bool sameState = ((flags ^ it->second.flags) & (pf::kFrozen | pf::kSleep)) == 0;
        if (dx * dx + dy * dy + dz * dz <= kResendCm2 && sameState) return;
    }
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorRotation(item, rot)) return;
    coop::net::PropSnapPosPayload p{};
    p.eid = key;
    p.locX = cur.X; p.locY = cur.Y; p.locZ = cur.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    p.physFlags = flags;
    const coop::prop_drive_host::DriveContext ctx = coop::prop_drive_host::ContextOf(item, key);
    p.driveGen  = ctx.gen;
    p.driveOpen = ctx.open ? 1 : 0;
    if (!s.SendReliable(coop::net::ReliableKind::PropSnapPos, &p, sizeof(p))) return;
    g_lastSent[key] = Sent{cur, flags};
    ++g_sentCount;
    if (!g_firstSaid) {
        g_firstSaid = true;
        UE_LOGI("[OPEN-CONT] HOST sent the first contents correction: eid=%u pos=(%.0f,%.0f,%.0f) flags=0x%02x",
                key, cur.X, cur.Y, cur.Z, static_cast<unsigned>(flags));
    }
}

}  // namespace

void Tick(coop::net::Session& session) {
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    const uint64_t now = NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kPeriodMs;
    if (!session.AnyWorldReadyPeer()) return;  // no client could take a correction

    std::vector<void*> containers;
    Ctx ctx{&session, &containers};
    for (int i = 0; i < kClassCount; ++i) {
        if (!g_cls[i]) g_cls[i] = OI::ClassByName(kClasses[i]);
        if (g_cls[i]) OI::ForEachInstance(g_cls[i], &Collect, &ctx);
    }
    if (containers.empty()) return;
    if (g_itemsOff == -2) {
        g_itemsOff = R::FindPropertyOffset(R::ClassOf(containers[0]), L"itemsInside");
        if (g_itemsOff < 0)
            UE_LOGW("[OPEN-CONT] prop_openContainer_C.itemsInside did not resolve -- contents are not corrected");
    }
    if (g_itemsOff < 0) return;
    for (void* c : containers) {
        const ActorArray* arr = reinterpret_cast<const ActorArray*>(reinterpret_cast<const uint8_t*>(c) + g_itemsOff);
        if (!arr->data || arr->num <= 0 || arr->num > kMaxItemsPerContainer || arr->num > arr->max) continue;
        // A container a client carries is moved here by that client's stream, and its own copy carries the
        // contents it attached there: a correction from this copy would trail it and drag them back. Its
        // contents are corrected once the client lets it go.
        const coop::element::ElementId ceid = coop::prop_element_tracker::GetPropElementIdForActor(c);
        if (ceid != coop::element::kInvalidId && ceid != 0 &&
            coop::prop_drive_host::ContextOf(c, static_cast<uint32_t>(ceid)).open)
            continue;
        for (int32_t k = 0; k < arr->num; ++k) {
            void* item = arr->data[k];
            if (!item || !R::IsLive(item)) continue;
            SendItem(session, item);
        }
    }
}

void OnDisconnect() {
    if (g_sentCount)
        UE_LOGI("[OPEN-CONT] session end -- %llu contents corrections sent",
                static_cast<unsigned long long>(g_sentCount));
    g_lastSent.clear();
    g_sentCount = 0;
    g_nextMs = 0;
    g_firstSaid = false;
}

}  // namespace coop::open_container_sync
