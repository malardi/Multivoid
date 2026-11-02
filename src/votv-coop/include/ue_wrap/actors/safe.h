// ue_wrap/actors/safe.h -- the safe (prop_safe_C and below): `locked`, and `hacked`, the game's own
// unlock, which is guarded by `locked` and creates the contents once. Ported from the WatchersOfTheVoid fork
// (MIT). Game thread.

#pragma once

namespace ue_wrap::safe {

bool IsSafe(void* actor);
bool ReadLocked(void* actor, bool& locked);
// Unlock through the game's own `hacked`. A safe already open answers true and creates nothing again.
bool Unlock(void* actor);

}  // namespace ue_wrap::safe
