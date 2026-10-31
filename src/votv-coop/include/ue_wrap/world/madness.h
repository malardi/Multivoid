// ue_wrap/world/madness.h -- engine access for the Madness Combat event (the funny console's madness.combat):
// the event master (madnessCombatMaster_C), whose MC_music component plays the event's music and whose killcount
// widget (ui_madnessCombat_C) shows the kills; and, on a machine that does not run the event, the same music and
// counter shown on their own. Presentation only: no master, grunt or weapon is made, and the widget's addCount
// (which would spawn more enemies) is never called. Engine-wrapper layer (principle 7). Game thread.
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/ue_wrap/world/madness.cpp (MIT).

#pragma once

#include <cstdint>

namespace ue_wrap::madness {

// The event as its master shows it. `elapsed` is the master's age in seconds (the music starts with it).
struct State {
    bool    active  = false;  // a master stands in the current world
    bool    hud     = false;  // its killcount widget exists
    bool    playing = false;  // its music component is playing
    int32_t kills   = 0;      // the widget's count
    float   elapsed = 0.f;
};

// The current world's event master, or null. One indexed class lookup, no object-array walk.
void* FindMaster();

// The master's state. False when a member does not resolve or a read faulted. A master with no widget reads
// hud = false and kills 0, one with no music component playing = false.
bool Read(void* master, State& out);

// What a non-running machine shows: the music it started and the widget it made, each held with its slot
// index, so a reaped object is never touched.
struct View {
    void*    audio      = nullptr;
    int32_t  audioIdx   = -1;
    void*    widget     = nullptr;
    int32_t  widgetIdx  = -1;
    int32_t  kills      = -1;   // the count last written to the widget
    bool     started    = false;  // the music was started for this playing stretch (it is never restarted in it)
    uint32_t world      = 0;    // world_identity::Generation() the view was made in
};

// Bring `view` to `state`: the music started at `startTime` seconds when it should play and does not, stopped
// when it should not; the widget made and put on the screen when the HUD shows, its count written when it
// changed, removed when it does not. An inactive state clears the view. Something made in another world is
// dropped, not touched. True when the view matches the state.
bool Apply(View& view, const State& state, float startTime);

// Stop the music and remove the widget, and forget both.
void Clear(View& view);

}  // namespace ue_wrap::madness
