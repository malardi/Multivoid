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
//   (and the inspection verbs in lab_cmd.cpp: get/set/fields, call/gcall, around, dump, spawn, ...;
//   a target is player, gm, gi, slot, puppet:<n>, C:<Class>, near:<text>, chained as a/b/c)
//
// A command file is a script, run in order:
//   wait <seconds>                  pause the lines after it
//   waitworld <name> [timeout s]    pause until the current world's name holds <name>
// Input, as the player's keys reach the game (its own input events, so a background copy can act):
//   press <target> <action> [s]     InpActEvt_<action>: the press now, the release after s (0.15)
//   axis <target> <name> <v> <s>    InpAxisEvt_<name> with value v every tick for s seconds
//   release                         let every held key and axis go
// The world and the body:
//   daytime <hour>                  the day-night clock to that hour (the host's is the shared one)
//   vitals food=100 sleep=100 ...   write the player's vital rows
//   keepfed 1|0                     keep food, sleep and health full every 5 s
//   worlds [tag] / gc / pins        the UWorld census, a forced collection, the world-scoped GC pins
// Looking:
//   cam <target> [d] [yaw] [h]      a camera d cm from the target at compass yaw, h up, aimed at it;
//                                   the view goes to it until `cam off` (shot then shows the target)
//   goto <target> [d] [dz]          stand d cm in front of the target (its forward), look at it
//   funcs <target|Class> [text]     the class's functions and their parameters into <tag>_funcs.txt
//   window show|hide                a copy launched hidden (lab_window.h) brought on screen, or parked
// Screens, without the mouse (the copy's window is off-screen; the cursor is the owner's):
//   widgets [target|-] [text]       the live user widgets, or the widgets inside one, into <tag>_widgets.txt
//   click <target> [delegate]       the button's OnClicked (or the named delegate) broadcast, as a click
//   focus <target>                  the widget takes the keyboard focus (an edit box, before type)
//   type <text>                     characters and {enter} {tab} {back} {esc} ... posted to the window
//   drag <target> <action> <s> <axis>=<v> ...   the action held while the axes are pushed (a brush stroke)
//   (target obj:<Name>[@<Class>]: a live object by its own name, inside an object of that class)
// Comparing peers:
//   watch <name> <target> <prop|@loc|@rot|@live|@class>   "watch <name> <value>" in the state file each
//                                   second; the same name on host and client is the lab's compare pair
//   unwatch <name|all>
// The state file also carries "smooth <slot> ..." per puppet: frames, speed, frame-to-frame change of
// speed, jumps and frames stood still while walking, over the last second.

#pragma once

namespace coop::dev::lab_cmd {

void Tick();

}  // namespace coop::dev::lab_cmd
