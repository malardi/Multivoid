// coop/props/container_slice_wire.h -- the BYTES of one world container's contents slice, and
// nothing else: `[u8 op=0][u32 eid][u64 baseHash][u16 n]` then n records in the
// coop/items/save_record_wire grammar (op=1, the refusal answer, names a slice in place of the
// base; see PackRefusal). Pure byte work, no engine access -- what a record MEANS
// (is it a nested container, may this author write it) belongs to the lane that owns the
// container, coop/props/container_contents_sync.
//
// The header and the records are parsed by SEPARATE calls on purpose: the host decides whether to
// accept a client's slice from the eid and the base hash alone, and a refused slice must not have
// cost a full record parse first.
//
// `baseHash` is the last host truth the author had applied for this eid (0 for the host itself, or
// for an author that never applied anything). It lets the host tell "the client edited the world I
// published" from "the client edited a world that has moved on", without which a full-slice write
// from a stale author would silently erase a host addition the author had not received.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstdint>
#include <vector>

namespace coop::props::container_slice_wire {

// A container with more records than this is neither shipped nor accepted: the blob would approach
// the transport ceiling, and a truncated blob is a silent lie. Real containers hold single digits.
inline constexpr size_t kMaxRecords = 512;

// The base a client sends with the first slice of a container it threw into the world: the host
// built its copy from the client's intent and has published nothing for it, so there is no truth to
// have edited from. The host decides by the transfer it awaits from that author, not by this value
// (container_contents_sync); a host that awaits nothing judges it like any stale base.
inline constexpr uint64_t kBirthBase = ~0ull;

std::vector<uint8_t> Pack(uint32_t eid, uint64_t baseHash,
                          const std::vector<ue_wrap::save_record::SaveRecord>& recs);

// The host's truth sent back to an author whose slice it refused: the same records, op 1, and in
// place of the base the id of the refused slice (SliceId of the blob the author sent). A contents
// slice cannot say it: a host fan-out carries base 0 and a relayed slice its author's base, so the
// author could not tell "your edit was refused" from "the world moved on" -- and only the first may
// take back what that edit put in the author's own inventory (coop/props/container_take_ledger).
std::vector<uint8_t> PackRefusal(uint32_t eid, uint64_t refusedSlice,
                                 const std::vector<ue_wrap::save_record::SaveRecord>& recs);

// The id of one slice as it travelled: the hash of its bytes, which the reliable transport delivers
// unchanged, so the author and the host compute the same value from their own copies.
uint64_t SliceId(const std::vector<uint8_t>& blob);

// The hash every gate and compare-and-swap uses, over a pack with baseHash zeroed, so it names the
// contents alone: the same records hash the same whichever peer authored them and whatever base
// they edited from.
uint64_t ContentHash(uint32_t eid, const std::vector<ue_wrap::save_record::SaveRecord>& recs);

// False on a blob that is not one of ours, or too short. Leaves `o` at the record count. A refusal
// answer sets `outRefusal`, and `outBaseHash` is then the refused slice's id.
bool ParseHeader(const std::vector<uint8_t>& b, size_t& o, uint32_t& outEid, uint64_t& outBaseHash,
                 bool& outRefusal);

// The declared count and the records after it. False on an over-cap count, a count the remaining
// bytes cannot feed, or a malformed record; `outWhy` then names which for the caller's log line.
bool ParseRecords(const std::vector<uint8_t>& b, size_t& o,
                  std::vector<ue_wrap::save_record::SaveRecord>& out, const char** outWhy);

}  // namespace coop::props::container_slice_wire
