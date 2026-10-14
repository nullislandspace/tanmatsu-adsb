#pragma once

#include <stdbool.h>

#include "../data/aircraft.h"

enum NavState {
    NAV_VIEW_BROWSING,      // Arrows switch views, Down/Up/Enter picks an aircraft
    NAV_AIRCRAFT_SELECTED,  // Arrows cycle aircraft, Enter opens detail, Esc goes back
    NAV_DETAIL_CARD_OPEN,   // Arrows scroll the card, Esc dismisses it
};

// Initialize navigation controller (call after views_init and keyboard_input_start)
void input_nav_init(AircraftList *list);

// Poll the keyboard lanes and update the UI (call from an LVGL timer, ~50ms)
void input_nav_tick();

// Handle an Esc press. Returns true when navigation consumed it.
bool input_nav_handle_back();

// Query selection state (used by view draw callbacks for highlighting)
int  input_nav_get_selected_index();   // -1 if none
bool input_nav_is_selecting();         // true when in AIRCRAFT_SELECTED state
NavState input_nav_get_state();
