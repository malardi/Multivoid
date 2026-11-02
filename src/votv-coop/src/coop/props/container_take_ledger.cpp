// coop/props/container_take_ledger.cpp -- see coop/props/container_take_ledger.h.

#include "coop/props/container_take_ledger.h"

#include "coop/props/container_slice_wire.h"  // kMaxRecords
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop.h"  // GetInteractableKeyString: the key the put actor carries
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/hotbar/icons.h"  // Rebuild: the quick-slot bar after a record is taken back

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

namespace coop::props::container_take_ledger {
namespace {

namespace ci  = ue_wrap::container_inventory;
namespace cw  = coop::props::container_slice_wire;
namespace INV = ue_wrap::inventory;
namespace SR  = ue_wrap::save_record;

struct Take {
    uint32_t eid = 0;
    bool put = false;                         // a put: putRecord goes back to the store on a refusal
    SR::SaveRecord putRecord;
    std::wstring takenClass;                  // the container record's class
    std::vector<std::wstring> carriedBefore;  // the keys the carried store held at the take
    bool resolved = false;                    // the carried store has been looked at since
    std::wstring gainedKey;                   // the carried record the take became; empty = none
    uint64_t slice = 0;                       // the slice that carried it to the host; 0 = not shipped
    uint64_t shipSeq = 0;                     // the order of that ship among this peer's ships
    uint64_t atMs = 0;
};

// Oldest first. A deque because the settling erases from anywhere and the cap pops the front.
std::deque<Take> g_takes;
uint64_t g_shipSeq = 0;

// The record this player's own store gave up last, until a put claims it or it ages out. The pair the
// inventory screen runs is one Blueprint call, so a put follows its take within the same frame; the
// bound only keeps a take that went elsewhere (a drop, a hold) from being claimed by a later put.
SR::SaveRecord g_lastPersonal;
uint64_t g_lastPersonalMs = 0;
constexpr uint64_t kPutPairMs = 2000;

void Push(Take&& t) {
    if (g_takes.size() >= kMaxTakes) g_takes.pop_front();
    g_takes.push_back(std::move(t));
}

bool Claimed(const std::wstring& key) {
    for (const Take& t : g_takes)
        if (t.gainedKey == key) return true;
    return false;
}

}  // namespace

void NoteTake(uint32_t eid, void* inv, int32_t index, uint64_t nowMs) {
    std::vector<SR::SaveRecord> recs;
    if (!ci::ReadContents(inv, recs, cw::kMaxRecords) || index < 0 ||
        index >= static_cast<int32_t>(recs.size()))
        return;   // takeObj refuses an invalid index too: nothing leaves, nothing to take back
    Take t;
    t.eid = eid;
    t.takenClass = recs[static_cast<size_t>(index)].className;
    t.atMs = nowMs;
    // What is carried NOW, before putObjectInventory2 adds the take: the record the take becomes is
    // the one of its class that is new at the next tick. Not its key: the game hands the player's store
    // the spawned actor's getData before loadData restored the container record's key, so the carried
    // record's key is the fresh one the spawn minted.
    INV::LivePersonalStore live;
    if (INV::ReadLivePersonalStore(live)) {
        t.carriedBefore.reserve(live.records.size());
        for (const SR::SaveRecord& r : live.records) t.carriedBefore.push_back(r.key);
    } else {
        t.resolved = true;   // no store to diff against later: there is nothing this take can name
    }
    Push(std::move(t));
}

void NotePersonalTake(void* inv, int32_t index, uint64_t nowMs) {
    std::vector<SR::SaveRecord> recs;
    if (!ci::ReadContents(inv, recs, cw::kMaxRecords) || index < 0 ||
        index >= static_cast<int32_t>(recs.size()))
        return;
    g_lastPersonal = std::move(recs[static_cast<size_t>(index)]);
    g_lastPersonalMs = nowMs;
}

void NotePut(uint32_t eid, void* actor, uint64_t nowMs) {
    if (!actor || g_lastPersonalMs == 0 || nowMs - g_lastPersonalMs > kPutPairMs) return;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    if (key.empty() || key == L"None" || key != g_lastPersonal.key) return;
    // A held or thrown world prop put in is not held here: its actor's end crossed to the host as a
    // destroy of its own, and what giving it back would take -- a world birth -- is the drop-intent
    // lane's, not this one's.
    Take t;
    t.eid = eid;
    t.put = true;
    t.putRecord = std::move(g_lastPersonal);
    t.takenClass = t.putRecord.className;
    t.resolved = true;
    t.atMs = nowMs;
    g_lastPersonal = SR::SaveRecord{};
    g_lastPersonalMs = 0;
    UE_LOGI("container_take_ledger: a put of '%ls' (key '%ls') into eid=%u from the inventory -- held "
            "until the host answers its slice", t.takenClass.c_str(), key.c_str(), eid);
    Push(std::move(t));
}

void Tick(uint64_t nowMs) {
    if (g_takes.empty()) return;
    INV::LivePersonalStore live;
    bool looked = false, read = false;
    for (Take& t : g_takes) {
        if (t.resolved) continue;
        if (!looked) { looked = true; read = INV::ReadLivePersonalStore(live); }
        if (!read) break;   // the next tick looks again; age settles a store that never reads
        for (const SR::SaveRecord& r : live.records) {
            if (r.className != t.takenClass || r.key.empty() || r.key == L"None") continue;
            if (std::find(t.carriedBefore.begin(), t.carriedBefore.end(), r.key) != t.carriedBefore.end())
                continue;
            if (Claimed(r.key)) continue;   // two takes of one class in one frame name two records
            t.gainedKey = r.key;
            break;
        }
        t.resolved = true;
        t.carriedBefore.clear();
        t.carriedBefore.shrink_to_fit();
        if (!t.gainedKey.empty())
            UE_LOGI("container_take_ledger: a take of '%ls' from eid=%u is carried as key '%ls' -- held "
                    "until the host answers its slice", t.takenClass.c_str(), t.eid, t.gainedKey.c_str());
    }
    g_takes.erase(std::remove_if(g_takes.begin(), g_takes.end(),
                                 [nowMs](const Take& t) { return nowMs - t.atMs > kSettleMs; }),
                  g_takes.end());
}

void NoteShipped(uint32_t eid, uint64_t sliceId) {
    bool any = false;
    for (Take& t : g_takes) {
        if (t.eid != eid || t.slice != 0) continue;
        if (!any) { any = true; ++g_shipSeq; }
        t.slice = sliceId;
        t.shipSeq = g_shipSeq;
    }
}

size_t OnRefused(uint32_t eid, uint64_t sliceId) {
    // The refused slice's takes, and the order it shipped in. Every slice of this container shipped
    // before it is settled: the host judges one author's slices in the order they were sent, and a
    // refusal of any of them would have come first. Silence was their acceptance.
    uint64_t seq = 0;
    std::vector<Take> refused;
    for (const Take& t : g_takes) {
        if (t.eid != eid || t.slice != sliceId || t.slice == 0) continue;
        refused.push_back(t);
        seq = t.shipSeq;
    }
    if (refused.empty()) {
        // The refused edit took nothing this peer carries: it put something in, or its takes stayed in
        // the world. The host's truth that came with the refusal is the whole answer.
        UE_LOGI("container_take_ledger: the host refused slice %016llx of eid=%u -- it carried no take "
                "of this player's", static_cast<unsigned long long>(sliceId), eid);
        return 0;
    }
    g_takes.erase(std::remove_if(g_takes.begin(), g_takes.end(),
                                 [&](const Take& t) {
                                     return t.eid == eid && t.slice != 0 && t.shipSeq <= seq;
                                 }),
                  g_takes.end());
    size_t back = 0;
    for (const Take& t : refused) {
        if (t.put) {
            if (INV::AddCarriedRecord(t.putRecord)) {
                ++back;
                UE_LOGI("container_take_ledger: refused put of '%ls' into eid=%u RETURNED to the inventory "
                        "(key '%ls') -- the host's container never held it", t.takenClass.c_str(), eid,
                        t.putRecord.key.c_str());
            } else {
                UE_LOGW("container_take_ledger: refused put of '%ls' into eid=%u could NOT be returned to "
                        "the inventory (key '%ls': carried already, or the store does not resolve)",
                        t.takenClass.c_str(), eid, t.putRecord.key.c_str());
            }
            continue;
        }
        if (t.gainedKey.empty()) {
            // Taken into the world rather than the inventory, or the inventory had no room for it.
            UE_LOGI("container_take_ledger: refused take of '%ls' from eid=%u was never carried -- "
                    "nothing to take back", t.takenClass.c_str(), eid);
            continue;
        }
        if (INV::RemoveCarriedByKey(t.gainedKey)) {
            ++back;
            UE_LOGI("container_take_ledger: refused take of '%ls' from eid=%u TAKEN BACK out of the "
                    "inventory (key '%ls') -- the host's container keeps it", t.takenClass.c_str(), eid,
                    t.gainedKey.c_str());
        } else {
            // Moved on before the answer came: dropped, held, put elsewhere. What it became is that
            // lane's to settle; this one can no longer name it.
            UE_LOGW("container_take_ledger: refused take of '%ls' from eid=%u is no longer carried "
                    "(key '%ls') -- NOT taken back", t.takenClass.c_str(), eid, t.gainedKey.c_str());
        }
    }
    if (back) {
        // The bar lists the carried store and was built with the record just taken back.
        ue_wrap::hotbar::State bar;
        if (ue_wrap::hotbar::Read(bar)) ue_wrap::hotbar::Rebuild(bar);
    }
    return back;
}

void Reset() {
    g_takes.clear();
    g_shipSeq = 0;
    g_lastPersonal = SR::SaveRecord{};
    g_lastPersonalMs = 0;
}

}  // namespace coop::props::container_take_ledger
