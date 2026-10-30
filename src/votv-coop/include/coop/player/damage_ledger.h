// coop/player/damage_ledger.h -- what hurt this machine's player, kept so a death can name its cause.
//
// Every Add Player Damage on the LOCAL player is recorded (amount, source class, the Blueprint that called
// it), the first of each kind is logged, and a hit that takes health to zero logs the last hits before it as
// [DEATH-CAUSE]. A puppet's damage is refused elsewhere (coop/player/player_damage); here it is only counted.
// A players' report ("the heart pounds, then a death with no visible cause") had no log trail: this is it.
#pragma once

namespace coop::player::damage_ledger {

// Game thread, every pump tick: registers the watch once (a cheap latched check afterwards).
void Tick();

}  // namespace coop::player::damage_ledger
