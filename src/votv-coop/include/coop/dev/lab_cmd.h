// coop/dev/lab_cmd.h -- the test lab's command channel: a copy of the game launched by the lab
// (VOTVCOOP_LAB_DIR and VOTVCOOP_LAB_TAG in its environment) reads numbered commands from
// <dir>\<tag>_cmd.txt, answers each in <tag>_out.txt and keeps <tag>_state.txt current. Inert in a
// launch without the variables (every player's). Game thread (the harness frame tail).
//
// Commands, one per line, "<id> <verb> <args>"; an id at or below the last one done is skipped:
//   tp x y z [yaw]     teleport this peer's player
//   travel <level>     lib_C::loadLevel(level) with the game's entry pass, as the game's own entries
//   exec <command>     a console command
//   shot <label>       a window capture into coop-screenshots
//   callp <function>   a no-parameter function on this peer's player
//   callgm <function>  a no-parameter function on the gameplay mode
//   log <text>         a marker line in multivoid.log

#pragma once

namespace coop::dev::lab_cmd {

void Tick();

}  // namespace coop::dev::lab_cmd
