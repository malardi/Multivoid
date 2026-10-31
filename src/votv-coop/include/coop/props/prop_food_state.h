// coop/props/prop_food_state.h -- a food's save record, made safe to move between peers.
//
// Aprop_food_C::getData writes the use count (ints[0][0]) and, in floats group 0, [lifespan,
// temperature, ripeness, a GetTimeSeconds stamp]; loadData then runs a catch-up of `its own clock -
// the stamp`. Two peers' worlds start at different moments, so a record moved between them rotted or
// warmed the food by that gap. The receiver now rebases the stamp to its own clock first, which makes
// the catch-up zero, so the family rides the record lane at every birth (a place, a drop, a join) and
// a half-eaten meal no longer arrives whole. A cooking food's progress, which the native record lacks,
// rides a tagged float group of our record body, stripped before loadData and written after it.
// The drift between births is not carried; a bite is (coop/props/prop_record_refresh). Pocket and
// container records are not touched -- docs/scope.md. Partly adapted from ElvardFIC/WatchersOfTheVoid
// (MIT), the cooking lineage test and its progress field:
// https://github.com/ElvardFIC/WatchersOfTheVoid/blob/HEAD/src/votv-coop/src/ue_wrap/actors/cooking_food.cpp

#pragma once

#include <cstdint>

namespace ue_wrap::save_record { struct SaveRecord; }

namespace coop::props::prop_food_state {

// True for Aprop_food_C and every class below it. Game thread.
bool IsFood(void* actor);

// What a record carries beyond the native codec, held across the apply.
struct Carried {
    bool  haveCook = false;
    float cookProgress = 0.f;
};

// A food's record about to leave this peer: its cooking progress, when it has one, is appended as
// the tagged group. Anything not a food passes untouched. Game thread.
void DecorateCapture(void* actor, ue_wrap::save_record::SaveRecord& rec);

// A record about to be applied to `actor` through its loadData: the tagged group is taken out into
// `out`, and a food's stamp is rebased to this peer's clock. False when the record is a food's and
// this peer's clock cannot be read -- such a record must not land with a foreign stamp; the caller
// keeps it for a later try. True and nothing changed for anything not a food. Game thread.
bool PrepareApply(void* actor, ue_wrap::save_record::SaveRecord& rec, Carried& out);

// After loadData: the carried cooking progress written to the field. Game thread.
void FinishApply(void* actor, const Carried& carried);

}  // namespace coop::props::prop_food_state
