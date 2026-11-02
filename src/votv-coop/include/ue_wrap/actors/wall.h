// ue_wrap/actors/wall.h -- the player-built concrete walls: an outline (customWall_unfinished_C) the wall builder
// (prop_wallbuilder_C::addWall) places, fed building material until its buildWall makes the wall; the wall
// (customWall_C, customWall_rad_C) and the wet concrete one (customWall_wetConcrete_C), which dries into a wall.
//
// Each is an actor_save with a key, saved by getData and restored by loadData, so its whole state is one save record:
// the outline's size, progress, shape and material row, a wall's health, a wet wall's drying (two floats the native
// getData leaves out, appended here). Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/ue_wrap/actors/
// placed_save.cpp, placed_save_record.cpp, wall_build.cpp, wall_drying.cpp and wall_creative.cpp (MIT), the wall
// family alone: no plant, hose, faucet, hole or marker. Game thread.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <functional>
#include <string>

namespace ue_wrap::wall {

enum class Kind : uint8_t { None, Outline, Wall };

// The outline's material overlap, the native body that spends a building material on it.
inline constexpr const wchar_t* kOverlap =
    L"BndEvt__customWall_unfinished_overlap_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature";

Kind KindOf(void* actor);
Kind ClassKind(const std::wstring& className);
bool IsWet(void* actor);

// The four wall classes, for a census.
inline constexpr const wchar_t* kClasses[] = {L"customWall_unfinished_C", L"customWall_C", L"customWall_rad_C",
                                              L"customWall_wetConcrete_C"};

// `function` is a placement of a wall of `kind` by the game's own factories: the wall builder's addWall, an
// outline's buildWall, a wet wall's drying. Not world generation, not a save load.
bool IsPlacementCaller(Kind kind, void* function);

// The record as getData makes it, a wet wall's drying appended. False when the call failed.
bool Capture(void* actor, save_record::SaveRecord& record);
// Keep only the wall family's fields, checked; false for a malformed record.
bool Normalize(const save_record::SaveRecord& in, save_record::SaveRecord& out);

// The key the actor holds now, never minted. Empty when it has none.
std::wstring Key(void* actor);
// Write the key and register it with the gamemode (getKey).
bool SetKey(void* actor, const std::wstring& key);

// Spawn the record's actor, or paint the record onto one (scale first, loadData, the drying, the outline's material
// row, the key).
void* Spawn(const save_record::SaveRecord& record);
bool Apply(void* actor, const save_record::SaveRecord& record);

// The host's replay of a measured material overlap, only while the two still overlap. True when handled or no longer
// overlapping; false when it should be tried again.
bool Feed(void* outline, void* material, const std::function<bool()>& current);

// The native damage body on a wall, its origin as the hit; the debris and destruction are the graph's.
bool Damage(void* wall, float amount);

// The creative build: the wall builder's addWall asking buildWall(isCheat) on an outline, allowed while the world is
// custom (lib_C::isCustom).
bool IsBuilderCall(void* tool, void* function);
bool CreativeAllowed(void* context, bool& allowed);
bool FinishCreative(void* outline);

}  // namespace ue_wrap::wall
