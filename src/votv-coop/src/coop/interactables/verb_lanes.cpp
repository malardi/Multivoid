// coop/interactables/verb_lanes.cpp -- see coop/interactables/verb_lanes.h.

#include "coop/interactables/verb_lanes.h"

#include "coop/interactables/camera_toggle_intent.h"
#include "coop/interactables/desk_verb_intent.h"
#include "coop/interactables/door_state_verbs.h"
#include "coop/interactables/door_verb_intent.h"
#include "coop/interactables/drone_call_intent.h"
#include "coop/interactables/eraser_press_intent.h"
#include "coop/interactables/keypad_verbs.h"
#include "coop/interactables/lightgroup_verbs.h"
#include "coop/interactables/swinger_pose_sync.h"
#include "coop/interactables/toggle_verbs.h"
#include "coop/items/point_sack_intent.h"
#include "coop/net/session.h"
#include "coop/props/open_container_sync.h"
#include "coop/world/blood_decal_sync.h"
#include "coop/creatures/walker_mirror.h"
#include "coop/world/dimension_travel.h"
#include "coop/props/pry_break_sync.h"

#include "ue_wrap/core/walk_timer.h"  // per-lane [WALK-TIME] attribution (diagnostic)

namespace coop::verb_lanes {

void Install(coop::net::Session& session) {
    coop::drone_call_intent::Install(&session);  // a client's press of the drone console is run by the host
    coop::eraser_press_intent::Install(&session);  // a client's press of the drive eraser is run by the host
    coop::door_verb_intent::Install(&session);   // a client's press, hit or pry of a base door is run by the host
    coop::camera_toggle_intent::Install(&session);  // a client's use of a security camera or its laptop light button is run by the host
    coop::door_state_verbs::Install(&session);   // a door's open state moves at doorOpen/doorClose: the host sends, a client refuses its own
    coop::lightgroup_verbs::Install(&session);   // a light group's state moves at its runTrigger: the host sends, a client refuses its own
    coop::toggle_verbs::Install(&session);       // a symmetric device's state goes at the verb that writes it: each peer sends its own
    coop::keypad_verbs::Install(&session);       // a keypad's verbs: the host sends each, a client's own entries run on the host
    coop::desk_verb_intent::Install(&session);   // a client's press on the desk's save family is run by the host
    coop::point_sack_intent::Install(&session);  // a client's opened point sack is paid by the host
    coop::pry_break_sync::Install(&session);     // a pried plank goes free everywhere; a client's container break is run by the host
    coop::blood_decal_sync::Install(&session);   // a blood splat lands on every peer
    coop::dimension_travel::Install(&session);   // a dimension: the session kept, the clients follow the host
    coop::walker_mirror::Install(&session);      // one walking mannequin: a client's brain off, the host's pose streamed
}

void Tick(coop::net::Session& session) {
    { ue_wrap::ScopedWalkTimer _w{"sync:drone_call"}; coop::drone_call_intent::Tick(session); }  // HOST: run one queued drone-console press a tick a client
    coop::eraser_press_intent::Tick(session);  // HOST: run one queued eraser press a tick a client, once its drive is seated
    { ue_wrap::ScopedWalkTimer _w{"sync:door_verb"}; coop::door_verb_intent::Tick(session); }    // HOST: run one queued door verb a tick a client
    { ue_wrap::ScopedWalkTimer _w{"sync:camera_toggle"}; coop::camera_toggle_intent::Tick(session); }  // settle its gates; HOST: run one queued camera request a tick a client
    coop::door_state_verbs::Tick();     // settle the door state watches
    coop::lightgroup_verbs::Tick();     // settle the light group watch
    coop::toggle_verbs::Tick();         // settle the toggle verb watches
    coop::point_sack_intent::Tick();    // settle the point sack watch
    coop::keypad_verbs::Tick(session);  // settle the keypad watches; HOST: run queued keypad intents
    { ue_wrap::ScopedWalkTimer _w{"sync:desk_verb"}; coop::desk_verb_intent::Tick(session); }  // settle its seams; HOST: run one queued desk press a tick a client
    { ue_wrap::ScopedWalkTimer _w{"sync:pry_break"}; coop::pry_break_sync::Tick(session); }  // send the planks a pry freed; HOST: run queued container breaks
    { ue_wrap::ScopedWalkTimer _w{"sync:open_container"}; coop::open_container_sync::Tick(session); }  // HOST: an open container's contents follow it on every peer
    coop::blood_decal_sync::Tick();     // settle the blood watch
    coop::walker_mirror::Install(&session);  // retry a watch not taken yet (idempotent)
    coop::walker_mirror::Tick(session);  // HOST: the walking mannequins' poses to the clients
    coop::swinger_pose_sync::Tick(session);  // a lid this peer swings by hand: its pose to the others
}

void OnDisconnect() {
    coop::drone_call_intent::OnDisconnect();  // its own console button stands
    coop::eraser_press_intent::OnDisconnect();  // its own eraser button stands
    coop::door_verb_intent::OnDisconnect();   // a door's own verbs run where they are used
    coop::camera_toggle_intent::OnDisconnect();  // a camera's own press runs where it is made
    coop::door_state_verbs::OnDisconnect();
    coop::lightgroup_verbs::OnDisconnect();
    coop::toggle_verbs::OnDisconnect();
    coop::keypad_verbs::OnDisconnect();
    coop::desk_verb_intent::OnDisconnect();
    coop::point_sack_intent::OnDisconnect();
    coop::pry_break_sync::OnDisconnect();
    coop::open_container_sync::OnDisconnect();
    coop::blood_decal_sync::OnDisconnect();
    coop::walker_mirror::OnDisconnect();
    coop::swinger_pose_sync::OnDisconnect();
    coop::dimension_travel::OnDisconnect();
}

}  // namespace coop::verb_lanes
