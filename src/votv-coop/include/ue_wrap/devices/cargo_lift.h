// ue_wrap/devices/cargo_lift.h -- the cargo lift (cargoLift_C): a placed controller whose platform is a
// physics body (`comp`) hung from an anchor (`attach`) and driven by a timeline (`move`), called by one of
// three buttons (`buttonsElev`, `buttonsElev1`, `buttonsElev2`). Ported from the WatchersOfTheVoid fork (MIT).
//
// Game thread throughout.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <functional>
#include <string>
#include <vector>

namespace ue_wrap::cargo_lift {

// What a peer needs to stand a lift where the host's stands: the trip's progress, the anchor's relative
// place, the platform's world pose, and the run flags.
struct State {
    float alpha = 0.f;
    FVector anchor{}, position{};
    FRotator rotation{};
    bool moving = false, direction = false;
};

// A live lift of the current world (never a CDO).
bool IsLift(void* actor);
// The lift's placed name: stable across peers, since the controller is a level actor.
std::wstring Key(void* actor);
// Every live lift of the current world, at most 16.
void Enumerate(std::vector<CachedObjRef>& out);
// Which button (0..2) `component` is on this lift, or -1.
int ButtonIndex(void* actor, void* component);
bool ButtonPosition(void* actor, int button, FVector& out);
bool Read(void* actor, State& out);
// Press `button` as `player`: the game's own action entry, which refuses a lift already moving, turns the
// direction and starts the trip. False when it did not run.
bool Activate(void* actor, int button, void* player);
// Stand the lift where the host's stands and, when the host's is moving, start the same trip natively. False
// while a trip of this peer's own is still running or the lift's parts are not up yet.
bool Replay(void* actor, const State& state, int button, void* player, const std::function<bool()>& stillCurrent);
// Finite and in range: a peer's numbers are checked before they move anything.
bool Valid(const State& state);

}  // namespace ue_wrap::cargo_lift
