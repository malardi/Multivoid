// coop/dev/lab_window.h -- the test lab's copies kept off the owner's screen. A copy the lab launches
// with VOTVCOOP_LAB_HIDE=1 parks every window it shows just beyond the right edge of the desktop,
// never activated and off the taskbar, so a lab run puts nothing in front of whoever is using the PC.
// Parked is not minimised: an off-screen window keeps its full size and DWM keeps composing it, so
// the game renders as usual and `shot` (PrintWindow) still reads real frames, where a minimised one
// would hand back nothing. Inert without the variable, which is every player's launch.
//
// The watcher is a thread of its own, started from the boot thread's first line (UE4SS starts the mod
// before the engine makes its window), so the game window is caught the moment it is shown; the lab's
// launcher also starts the process minimised without activation, which covers the few frames before.
// It keeps every window parked for the whole run: the engine moves its window again when it applies
// the resolution, and a window that comes back is parked again within a few tens of milliseconds.

#pragma once

#include <string>

namespace coop::dev::lab_window {

// From the boot thread; spawns the watcher when VOTVCOOP_LAB_HIDE=1, a no-op otherwise.
void Start();

// Whether the windows are parked now; false when the watcher never started.
bool Hidden();

// lab_cmd `window show|hide`: show puts each parked window back where the engine had it (still not
// activated: the owner's keyboard stays where it is), hide parks them again. False when the copy was
// not launched hidden (nothing to bring back).
bool SetHidden(bool hidden);

// The game's main window (the largest visible top-level window of this process), parked or not.
void* MainWindow();

// Typing into the game without focus: WM_CHAR per character to the main window, as the keyboard's
// translated characters reach it, and a key's down and up for the named keys ({enter} {tab} {back}
// {esc} {up} {down} {left} {right} {space}). Slate hands both to the widget that holds its keyboard
// focus, so an edit box is focused first (lab_cmd `focus`). The count of messages posted.
int PostText(const std::wstring& text);

}  // namespace coop::dev::lab_window
