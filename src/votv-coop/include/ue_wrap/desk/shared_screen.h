// ue_wrap/desk/shared_screen.h -- what the base computer's screen shows, as rows a peer can read and write.
//
// The base laptop and every portable PC render one ui_laptop_C (mainGamemode_C::laptop). Its rootswitch shows
// the boot page (0) or the desktop (1); only the desktop's subtree is read or written, because the boot page is
// the laptop's own latent startup and repainting it pins the computer to its loading page. A row is one widget
// of that subtree by its child path: its visibility, a switcher's active page, a scroll box's offset and a text
// block's text. Presentation only: no input handler, verb or button press is read or replayed. A collapsed or
// hidden widget's children are not walked, and a switcher's inactive pages are not either.
//
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/include/ue_wrap/desk/shared_screen.h (MIT).
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::shared_screen {

// Row flags: which of the optional members the widget carries.
inline constexpr uint8_t kHasTab    = 1;  // a widget switcher: `tab` is its active page
inline constexpr uint8_t kHasScroll = 2;  // a scroll box: `scroll` is its offset
inline constexpr uint8_t kHasText   = 4;  // a text block: `text` is what it shows

struct Row {
    std::string  path;            // "pc" then "/<child index>" per level, "/r" into a user widget's tree
    uint8_t      visibility = 0;  // ESlateVisibility
    uint8_t      flags = 0;       // kHas*
    int32_t      tab = 0;
    float        scroll = 0;
    std::wstring text;
};

// The desktop's rows, in path order. False when the widget or its desktop is not there, or a widget did not
// answer; `rows` is empty then.
bool Read(std::vector<Row>& rows);

// Write `rows` to this peer's desktop: each member that differs from what the widget shows now. A text the
// last Apply already wrote to that path is not written again, so a text block whose binding keeps its own
// value is not fought every call. True when every row found its widget and every write ran.
bool Apply(const std::vector<Row>& rows);

// Forget what Apply last wrote (the next Apply writes every text that differs). Game thread.
void ResetWritten();

}  // namespace ue_wrap::shared_screen
