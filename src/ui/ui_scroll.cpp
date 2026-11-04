#include <universal/bo1_ptrcast.h>
#include "ui_scroll.h"
#include "ui_shared.h"
#include "ui_utils.h"
#include "keycodes.h"
#include <client/cl_keys.h>
#include <client/screen_placement.h>
#include <client_mp/cl_scrn_mp.h>
#include <universal/dvar.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// mod: a menu's list that scrolls by whole rows (see ui_scroll.h).

static const float kOffscreen = 10000.0f;

static std::vector<UiScrollList *> s_lists;

void UI_Scroll_Add(UiScrollList *list)
{
    if ( std::find(s_lists.begin(), s_lists.end(), list) == s_lists.end() )
        s_lists.push_back(list);
}

void UI_Scroll_Remove(UiScrollList *list)
{
    s_lists.erase(std::remove(s_lists.begin(), s_lists.end(), list), s_lists.end());
}

std::string UI_Scroll_BarText(const std::string &name, float x, float top, float bottom, int horzAlign, int vertAlign,
    const std::string &scrollDvar)
{
    char text[1024];
    snprintf(text, sizeof(text),
        "itemDef { name \"%s_track\" type 1 rect %g %g 6 %g %d %d style 1 backcolor 0 0 0 0.4 decoration "
        "visible when ( dvarint( \"%s\" ) == 1 ) } "
        "itemDef { name \"%s_thumb\" type 1 rect %g %g 6 20 %d %d style 1 backcolor 1 1 1 0.75 decoration "
        "visible when ( dvarint( \"%s\" ) == 1 ) } ",
        name.c_str(), x, top, bottom - top, horzAlign, vertAlign, scrollDvar.c_str(), name.c_str(), x, top, horzAlign,
        vertAlign, scrollDvar.c_str());
    return text;
}

static bool Scroll_RowShown(const UiScrollRow &row)
{
    return row.shownDvar.empty() || Dvar_GetBool(row.shownDvar.c_str());
}

void UI_Scroll_Layout(int contextIndex, UiScrollList *list, bool force)
{
    // where each row goes (unscrolled), and the bottom of the last
    std::string layout;
    float next = list->flowY;
    float contentBottom = list->top;
    for ( UiScrollRow &row : list->rows )
    {
        row.shown = Scroll_RowShown(row);
        layout += row.shown ? '1' : '0';
        if ( !row.shown )
            continue;
        row.placedY = row.flow ? next : row.y;
        float h = list->rowH;
        for ( itemDef_s *item : row.items )
            h = std::max(h, item->window.rectClient.h);
        next = std::max(next, row.placedY + std::max(list->step, h + list->step - list->rowH));
        contentBottom = std::max(contentBottom, row.placedY + std::min(h, list->step * 2.0f));
    }
    const float total = contentBottom - list->top;
    const float view = list->bottom - list->top;
    list->maxFirst = total > view + 0.5f ? (int)ceilf((total - view) / list->step - 0.01f) : 0;
    list->first = std::max(0, std::min(list->first, list->maxFirst));
    char tail[32];
    snprintf(tail, sizeof(tail), " %d", list->first);
    layout += tail;
    if ( !force && layout == list->layout )
        return;
    list->layout = layout;
    const float scroll = list->step * (float)list->first;
    for ( UiScrollRow &row : list->rows )
    {
        for ( size_t i = 0; i < row.items.size(); ++i )
        {
            itemDef_s *item = row.items[i];
            if ( !row.shown )
            {
                item->window.rectClient.y = kOffscreen;
                continue;
            }
            // (one outside the frame: far below the screen, there for the keys)
            const float y = row.placedY + row.offsets[i];
            const float at = y - scroll;
            const bool in = at >= list->top - 0.5f && at + item->window.rectClient.h <= list->bottom + 0.5f;
            item->window.rectClient.y = in ? at : kOffscreen + y;
        }
    }
    if ( list->thumb && list->track )
    {
        const float thumbH = std::max(14.0f, view * view / std::max(total, view));
        list->thumb->window.rectClient.h = thumbH;
        list->thumb->window.rectClient.y =
            list->top + (list->maxFirst ? (view - thumbH) * (float)list->first / (float)list->maxFirst : 0.0f);
    }
    if ( !list->scrollDvar.empty() )
        Dvar_SetBoolByName(list->scrollDvar.c_str(), list->maxFirst > 0);
    Menu_UpdatePosition(contextIndex, list->menu);
}

// a rect on the screen (its alignment applied, as Rect_ContainsPoint does)
static rectDef_s Scroll_ScreenRect(const UiContext *dc, const rectDef_s &rect)
{
    rectDef_s r = rect;
    ScrPlace_ApplyRect(&scrPlaceView[dc->contextIndex], &r.x, &r.y, &r.w, &r.h, rect.horzAlign, rect.vertAlign);
    return r;
}

static float Scroll_CursorScreenY(const UiContext *dc)
{
    return (float)ScrPlace_ApplyY(&scrPlaceView[dc->contextIndex], dc->cursor.y, 4);
}

// the thumb with the cursor, held where it was grabbed
static void Scroll_DragTo(const UiContext *dc, UiScrollList *list)
{
    if ( !list->track || !list->thumb || list->maxFirst <= 0 )
        return;
    const rectDef_s track = Scroll_ScreenRect(dc, list->track->window.rect);
    const rectDef_s thumb = Scroll_ScreenRect(dc, list->thumb->window.rect);
    const float range = track.h - thumb.h;
    if ( range <= 0.0f )
        return;
    const float f = (Scroll_CursorScreenY(dc) - list->grabY - track.y) / range;
    list->first = (int)floorf(f * (float)list->maxFirst + 0.5f);
}

void UI_Scroll_Frame(int localClientNum, UiContext *dc, menuDef_t *menu)
{
    for ( UiScrollList *list : s_lists )
    {
        if ( list->menu != menu )
            continue;
        if ( list->dragging )
        {
            if ( Key_IsDown(localClientNum, K_MOUSE1) )
                Scroll_DragTo(dc, list);
            else
                list->dragging = false;
        }
        UI_Scroll_Layout(dc->contextIndex, list, false);
        // a row the focus got to (with the keys: maybe outside the frame) is scrolled into it
        for ( const UiScrollRow &row : list->rows )
        {
            if ( !row.shown || !row.focus || !(Window_GetDynamicFlags(dc->contextIndex, &row.focus->window) & 2) )
                continue;
            if ( row.focus != list->lastFocus )
            {
                const float scroll = list->step * (float)list->first;
                if ( row.placedY - scroll < list->top - 0.5f )
                    list->first = (int)floorf((row.placedY - list->top) / list->step + 0.01f);
                else if ( row.placedY + list->rowH - scroll > list->bottom + 0.5f )
                    list->first = (int)ceilf((row.placedY + list->rowH - list->bottom) / list->step - 0.01f);
                list->lastFocus = row.focus;
                UI_Scroll_Layout(dc->contextIndex, list, false);
            }
            break;
        }
    }
}

bool UI_Scroll_HandleKey(int localClientNum, UiContext *dc, menuDef_t *menu, int key, int down)
{
    for ( UiScrollList *list : s_lists )
    {
        if ( list->menu != menu )
            continue;
        if ( key == K_MOUSE1 && !down )
        {
            list->dragging = false;
            continue;
        }
        if ( !down || list->maxFirst <= 0 )
            continue;
        if ( key == K_MWHEELDOWN || key == K_MWHEELUP )
        {
            list->first += key == K_MWHEELDOWN ? 1 : -1;
            UI_Scroll_Layout(dc->contextIndex, list, false);
            return true;
        }
        if ( key != K_MOUSE1 || !list->track || !list->thumb )
            continue;
        // (the track, a little wider: it is thin)
        rectDef_s r = list->track->window.rect;
        r.x -= 4.0f;
        r.w += 8.0f;
        if ( !Rect_ContainsPoint(dc->contextIndex, &r, dc->cursor.x, dc->cursor.y) )
            continue;
        // the thumb: dragged; above or below it: a page up or down
        const rectDef_s thumb = Scroll_ScreenRect(dc, list->thumb->window.rect);
        const float y = Scroll_CursorScreenY(dc);
        const int page = std::max(1, (int)((list->bottom - list->top) / list->step) - 1);
        if ( y < thumb.y )
        {
            list->first -= page;
        }
        else if ( y > thumb.y + thumb.h )
        {
            list->first += page;
        }
        else
        {
            list->dragging = true;
            list->grabY = y - thumb.y;
        }
        UI_Scroll_Layout(dc->contextIndex, list, false);
        return true;
    }
    (void)localClientNum;
    return false;
}
