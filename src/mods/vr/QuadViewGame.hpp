// QuadViewGame.hpp -- what the quad view knows about the game it runs in.
//
// Everything else of the quad view (the three views, the periphery, the fovea, FSR 4, the exposure match, the guards)
// is the same for every RE Engine game built from this source with D3D12 and Single Frame Multipass (RE4, RE2, RE7):
// it works on the engine's cameras, layers, depth, motion vectors and GUI placement, not on the game's objects. What
// one game has and another has not goes here, so the rest stays free of game names.
#pragma once

#include <string_view>

namespace quadview::game {

// The game's own world elements that mark an item (drawn at the item's place in the world, not on the 2D interface):
// outside the fovea the wide view's copy of them reaches each eye at the item's depth.
inline bool is_item_beam(std::string_view gui_name) {
#if defined(RE4)
    return gui_name.starts_with("Gui_ui2130"); // the green beam over an item
#else
    (void)gui_name;
    return false;
#endif
}

// whether the game has such elements (the menu shows their option only then)
constexpr bool has_item_beams() {
#if defined(RE4)
    return true;
#else
    return false;
#endif
}

// [SMALL WINDOW fix125 22.11.2026] the game's window an engine view's size instead of an eye image's, on from the start
// (option "Small game window"). RE4: tested with fix123 / fix124 (22.11.2026: 0.07 ms of GPU per frame, the 2D cutscenes'
// canvas shows the eye image). Not in the other games: RE7's canvas shows its main menu with the quad view off (the
// window's own picture), not tried there.
constexpr bool small_window_default() {
#if defined(RE4)
    return true;
#else
    return false;
#endif
}

// [UI MOTION fix125 22.11.2026] the temporal passes fetch the interface along its own motion (option "Interface: its own
// motion"), on from the start. RE4: its HUD goes with the game camera and with the weapon while the scene moves under
// it, and its menus stand in front of an empty background (trails, faint writing; 19.11.2026). Not in the other games:
// RE7's menus were right with the scene's vectors (17.11.2026) and nothing is to change there untested.
constexpr bool ui_motion_default() {
#if defined(RE4)
    return true;
#else
    return false;
#endif
}

} // namespace quadview::game
