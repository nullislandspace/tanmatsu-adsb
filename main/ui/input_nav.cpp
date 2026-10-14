#include "input_nav.h"
#include "detail_card.h"
#include "map_view.h"
#include "views.h"
#include "../hal/keyboard_input.h"
#include <cstring>

#define NUM_VIEWS 4

static AircraftList *_list = nullptr;
static NavState _state = NAV_VIEW_BROWSING;
static int _selected_idx = -1;  // Index into the aircraft list

void input_nav_init(AircraftList *list) {
    _list = list;
    _state = NAV_VIEW_BROWSING;
    _selected_idx = -1;
}

static int get_aircraft_count() {
    if (!_list) return 0;
    return _list->count;
}

// Center the map on the currently selected aircraft and start tracking it.
static void focus_selected_aircraft() {
    if (!_list || _selected_idx < 0) return;

    if (_list->lock(pdMS_TO_TICKS(10))) {
        if (_selected_idx < _list->count) {
            Aircraft &ac = _list->aircraft[_selected_idx];
            map_view_center_on(ac.lat, ac.lon);
            map_view_track(ac.icao_hex);
        }
        _list->unlock();
    }
}

void input_nav_tick() {
    if (!_list) return;

    int view_delta = input_view_delta();
    int cycle_delta = input_cycle_delta();
    bool select = input_select_pressed();

    if (view_delta == 0 && cycle_delta == 0 && !select) return;

    switch (_state) {
        case NAV_VIEW_BROWSING: {
            if (view_delta != 0) {
                int current = views_get_active_index();
                int next = (current + view_delta) % NUM_VIEWS;
                if (next < 0) next += NUM_VIEWS;
                lv_tileview_set_tile_by_index(views_get_tileview(), next, 0, LV_ANIM_OFF);
                views_pause_cycle();
            }

            // Down/Up (or Enter) jumps straight into aircraft selection.
            if (cycle_delta != 0 || select) {
                int count = get_aircraft_count();
                if (count > 0) {
                    _selected_idx = (cycle_delta < 0) ? count - 1 : 0;
                    _state = NAV_AIRCRAFT_SELECTED;
                    views_pause_cycle();
                    focus_selected_aircraft();
                }
            }
            break;
        }

        case NAV_AIRCRAFT_SELECTED: {
            if (cycle_delta != 0) {
                int count = get_aircraft_count();
                if (count > 0) {
                    _selected_idx = (_selected_idx + cycle_delta) % count;
                    if (_selected_idx < 0) _selected_idx += count;
                    focus_selected_aircraft();
                }
            } else if (select) {
                if (_list->lock(pdMS_TO_TICKS(10))) {
                    if (_selected_idx >= 0 && _selected_idx < _list->count) {
                        Aircraft ac_copy = _list->aircraft[_selected_idx];
                        _list->unlock();
                        detail_card_show(&ac_copy);
                        _state = NAV_DETAIL_CARD_OPEN;
                    } else {
                        _list->unlock();
                    }
                }
            }
            break;
        }

        case NAV_DETAIL_CARD_OPEN: {
            if (select) {
                detail_card_hide();
                _state = NAV_AIRCRAFT_SELECTED;
            } else if (cycle_delta != 0) {
                detail_card_scroll(cycle_delta);
            }
            break;
        }
    }
}

bool input_nav_handle_back() {
    switch (_state) {
        case NAV_DETAIL_CARD_OPEN:
            detail_card_hide();
            _state = NAV_AIRCRAFT_SELECTED;
            return true;

        case NAV_AIRCRAFT_SELECTED:
            _selected_idx = -1;
            _state = NAV_VIEW_BROWSING;
            map_view_track(nullptr);
            return true;

        case NAV_VIEW_BROWSING:
        default:
            return false;
    }
}

int input_nav_get_selected_index() {
    return _selected_idx;
}

bool input_nav_is_selecting() {
    return _state == NAV_AIRCRAFT_SELECTED || _state == NAV_DETAIL_CARD_OPEN;
}

NavState input_nav_get_state() {
    return _state;
}
