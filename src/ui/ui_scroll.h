#pragma once
#include <string>
#include <vector>

struct UiContext;
struct menuDef_t;
struct itemDef_s;

// mod: a menu's list that scrolls by whole rows (src/ui/ui_scroll.cpp): Mission Select's (ui_levels.cpp) and the Mods
// menu's (ui_mods.cpp). The rows that fit in the list's frame are where they belong, scrolled; the others are far off
// the screen (still there for the keys: when the focus gets to one, the list scrolls to it). The mouse wheel scrolls it,
// and its scroll bar (two items of the menu: a track and a thumb, shown while the list scrolls) can be dragged, or
// clicked above or below the thumb for a page.

struct UiScrollRow
{
    std::vector<itemDef_s *> items;
    std::vector<float> offsets; // each item's y less the row's
    itemDef_s *focus = nullptr; // its item that takes the focus (the list scrolls to it)
    float y = 0.0f;             // its place in the list (unscrolled), unless it flows:
    bool flow = false;          // a step after the shown row before it (the first: at the list's flowY)
    std::string shownDvar;      // shown only while this dvar is on ("": always)
    float placedY = 0.0f;       // (where the last layout put it, unscrolled)
    bool shown = true;
};

struct UiScrollList
{
    menuDef_t *menu = nullptr;
    std::vector<UiScrollRow> rows;
    float top = 0.0f;       // the list's first row (where it is shown from)
    float bottom = 0.0f;    // the bottom of its frame (where rows stop being shown)
    float step = 19.0f;     // a row's step
    float rowH = 17.0f;     // a row's height
    float flowY = 0.0f;     // the first row that flows goes here, or after the rows before it
    itemDef_s *track = nullptr;
    itemDef_s *thumb = nullptr;
    std::string scrollDvar; // 1 while the list scrolls (the scroll bar's items are shown then)
    // (its state)
    int first = 0;
    int maxFirst = 0;
    std::string layout;
    itemDef_s *lastFocus = nullptr;
    bool dragging = false;
    float grabY = 0.0f;
};

// a list placed before each paint of its menu, with the wheel and its scroll bar (until removed)
void UI_Scroll_Add(UiScrollList *list);
void UI_Scroll_Remove(UiScrollList *list);
// its rows where they go now (force: even if nothing changed)
void UI_Scroll_Layout(int contextIndex, UiScrollList *list, bool force);
// Menu_Paint, before a menu is drawn: its lists placed, scrolled to the row with the focus, the thumb dragged
void UI_Scroll_Frame(int localClientNum, UiContext *dc, menuDef_t *menu);
// Menu_HandleKey, first: the mouse wheel and the scroll bar of its lists; true if the key was used
bool UI_Scroll_HandleKey(int localClientNum, UiContext *dc, menuDef_t *menu, int key, int down);
// the scroll bar's two items in a menu's text (<name>_track and <name>_thumb, 6 wide at x, from top to bottom), shown
// while the list's scrollDvar is 1
std::string UI_Scroll_BarText(const std::string &name, float x, float top, float bottom, int horzAlign, int vertAlign,
    const std::string &scrollDvar);
