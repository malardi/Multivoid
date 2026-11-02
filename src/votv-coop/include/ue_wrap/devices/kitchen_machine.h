// ue_wrap/devices/kitchen_machine.h -- the cooking state of the kitchen machines a peer can share: the microwave
// (prop_microwave_C), the mini oven (prop_minioven_C) and the grill (prop_bbq_C).
//
// Only the cooking state lives here: whether the machine runs, its mode, timer and power, and the grill's fire,
// coals, lid and burn. The doors of the microwave and the oven are swingers and move by swinger_pose_sync; the
// coffee maker and vending machine are ue_wrap/devices/coffee; the cremator's lever is toggle_verbs'. Nothing here
// touches a door. Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/ue_wrap/devices/kitchen_machine.cpp
// and grill.cpp (MIT), without their door, coffee and cremator parts.
//
// Every field is resolved by name on the instance's own class (a world reload makes the Blueprint class anew), a
// Blueprint bool with its mask. Game thread.

#pragma once

#include <cstdint>
#include <functional>

namespace ue_wrap::kitchen_machine {

enum class Kind : uint8_t { None = 0, Microwave = 1, MiniOven = 2, Grill = 3 };

// The bits of State::flags, the same on the wire (net::KitchenStatePayload::flags).
enum Flag : uint8_t {
    kActive  = 1,  // the microwave or oven runs; the grill burns
    kPowered = 2,  // the microwave or oven has power
    kLid     = 4,  // the grill's lid is on
    kCooking = 8,  // the grill's cooking loop plays
};

struct State {
    Kind     kind = Kind::None;
    uint8_t  flags = 0;
    int32_t  mode = 0;         // the microwave's timeMode, the oven's mode
    float    remaining = 0.f;  // the microwave's time
    float    timer = 0.f;      // the microwave's timer
    float    power = 0.f;      // the oven's power
    int32_t  coals = 0;        // the grill's coals, 0..16
    float    burn = 0.f;       // the grill's burn (its field `a`), 0..1
    float    volume = 0.f;     // the grill's cooking loop volume, 0..1
};

// The machine `actor` is, by its class or an ancestor; None for anything else, a dead object or null.
Kind KindOf(void* actor);

// A state's values are in the ranges the game keeps them in (a wire row is checked with it).
bool Valid(const State& s);

// The machine's cooking state now. False when a field did not resolve.
bool Read(void* actor, State& out);

// Paint `s` onto the machine: its fields, and the game's own repaint (the oven's and grill's upd and updLid), the
// microwave's light and loop, the grill's loop. Never starts cooking, never spends or makes an item: the tick that
// cooks is the host's. `current` is asked after every call into the game; false stops the paint.
bool Apply(void* actor, const State& s, const std::function<bool()>& current = {});

// The oven control the local player aims at, as the oven's hover sets it: 1 the switch, 2 the temperature, 0 none.
// 0 for the other machines.
uint8_t ReadControl(void* actor);

// A player's press of the machine's own action (actionOptionIndex) with `player`; on the oven the aimed control is
// set to `control` for the call and put back after. False for the grill, an action other than 4 (or the
// microwave's 8), or a call that did not run.
bool Press(void* actor, void* player, uint8_t action, uint8_t control);

// The grill's fire: lit by its own attemptIgnite (which needs coals), put out by writing `active` and its upd.
bool SetGrillFire(void* actor, bool burning);

// The grill's stock: its coal count (raised only, to at most 16) and its lid, written as fields with the game's
// updLid; no coal or lid item is spent or made. False when the fields did not resolve.
bool RaiseGrillCoals(void* actor, int32_t coals);
bool SetGrillLid(void* actor, bool on);

// The microwave's plate turns while it runs: a client, whose wave is the host's, turns it here, `seconds` the
// frame's time.
void Animate(void* actor, float seconds);

}  // namespace ue_wrap::kitchen_machine
