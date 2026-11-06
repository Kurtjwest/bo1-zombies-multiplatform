#include <universal/bo1_ptrcast.h>
#include "ui_levels.h"
#include "ui_main.h"
#include "ui_scroll.h"
#include "ui_shared.h"
#include "ui_shared_obj.h"
#include "ui_utils.h"
#include <client_mp/cl_scrn_mp.h>
#include <gfx_d3d/r_material.h>
#include <universal/com_expressions.h>
#include <universal/com_expressions_eval.h>
#include <qcommon/common.h>
#include <qcommon/com_mods.h>
#include <universal/dvar.h>
#include <universal/q_shared.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

// mod: the zombies menu's Mission Select (levels_zombie, a menu of frontend.ff) lists the map mods turned on in the
// Mods menu (ui_mods.cpp) after the game's maps, and scrolls when its rows don't fit.
//
// The game's menu stays as it is; when the front end's menus load, it gets more items:
// - a row per map of a map mod (shown while the mod is on; Overlook has two): items in the look of the list's first row (its bar, highlight,
//   button and texts: their type, rect, colors, material and font read from that row's items), a row's step below the
//   list's last row. Its button starts the map as the game's rows start theirs (ui_load_level, then the front end
//   script's startlevel);
// - the map's title and description where the game's show the focused map's (those read ui_level_loc_text, a
//   localization key a map mod hasn't got; they and the preview picture, "preview_" + ui_mapname, are hidden while a
//   map mod's row has the focus, and a panel saying it's a map mod is where the picture was);
// - a scroll bar right of the list, while the rows go past the bottom of the list's frame (ui_scroll.cpp: every item
//   of the list's column scrolls).
// The rows are a text menu as the Mods menu's (UI_LoadMenusFromMemory), whose items are moved into levels_zombie's
// list after the game's rows, so that the keys go from the last of those to the map mods'.

static const dvar_t *bo1_levels_title; // a map mod's name, when its row has the focus
static const dvar_t *bo1_levels_desc;  // its text
static const dvar_t *bo1_levels_panel; // 1 while a map mod's row has the focus: our panel instead of the game's

// a map mod's map's row (its items are a row of s_scroll)
struct LevelsModRow
{
    std::string folder;
    std::string map;
    std::string label;
    itemDef_s *button;
};

// the game's items showing the focused map, hidden while a map mod's row has the focus (hidden: their dynamic flag 4
// cleared; kept: whether it was set)
struct LevelsPanelItem
{
    itemDef_s *item;
    bool wasVisible;
};

static menuDef_t *s_menu;                   // levels_zombie, its items ours (s_items)
static std::vector<Ptr32<itemDef_s>> s_items;
static Ptr32<Ptr32<itemDef_s>> s_origItems; // the game's own list and count, put back at the UI's shutdown
static int s_origItemCount;
static std::vector<itemDef_s *> s_ourItems; // every item of ours (their expressions are freed at the shutdown)
static UiScrollList s_scroll;
static std::vector<LevelsModRow> s_rows;
static std::vector<LevelsPanelItem> s_stockPanel;
static bool s_stockPanelHidden;
static std::string s_panelRow; // the row whose name and text the panel has ("folder map")

static std::string Fmt(const char *format, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

// text in a quoted menu string: no quotes or backslashes, not a localization reference
static std::string Quote(const std::string &s)
{
    std::string q;
    for ( char c : s )
        q += c == '"' ? '\'' : c == '\\' ? '/' : c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
    while ( !q.empty() && q[0] == '@' )
        q.erase(0, 1);
    return q.size() > 900 ? q.substr(0, 900) : q;
}

static std::string Upper(const std::string &s)
{
    std::string u = s;
    for ( char &c : u )
        c = (char)toupper((unsigned char)c);
    return u;
}

static const ModInfo *FindMod(const std::string &folder)
{
    for ( const ModInfo &mod : Mods_List() )
    {
        if ( !I_stricmp(mod.folder.c_str(), folder.c_str()) )
            return &mod;
    }
    return nullptr;
}

// a map mod's rows in Mission Select: one per map, named as the mod when it has one map, else as the map
struct LevelsRowSpec
{
    const ModInfo *mod;
    std::string map;
    std::string label;
};

static std::vector<LevelsRowSpec> Levels_RowSpecs()
{
    std::vector<LevelsRowSpec> rows;
    for ( const ModInfo &mod : Mods_List() )
    {
        if ( !mod.mapMod )
            continue;
        for ( const ModMap &m : mod.maps )
            rows.push_back({ &mod, m.name, mod.maps.size() == 1 ? mod.name : m.label });
    }
    return rows;
}

// our row items' names: bo1_levels_r<row>_<item>
static std::string Levels_RowPrefix(int row)
{
    return Fmt("bo1_levels_r%d_", row);
}

// ----- reading the game's menu -----

extern const char *g_expOperatorNames[];

// whether an expression has a string constant (case aside) / calls a function of that name
static bool Exp_HasString(const ExpressionStatement *exp, const char *text)
{
    if ( !exp || !exp->filename || !exp->rpn )
        return false;
    for ( int i = 0; i < exp->numRpn && exp->rpn[i].type != 3; ++i )
    {
        const expressionRpn &r = exp->rpn[i];
        if ( r.type == 0 && r.data.constant.dataType == VAL_STRING && r.data.constant.internals.string
            && !I_stricmp(r.data.constant.internals.string, text) )
            return true;
    }
    return false;
}

static bool Exp_HasFunction(const ExpressionStatement *exp, const char *name)
{
    if ( !exp || !exp->filename || !exp->rpn )
        return false;
    for ( int i = 0; i < exp->numRpn && exp->rpn[i].type != 3; ++i )
    {
        const expressionRpn &r = exp->rpn[i];
        if ( r.type != 1 && r.type != 2 )
            continue;
        // (1: the menu file's id, 2: resolved, rpnFunctions' index: Expression_EvaluateRpn)
        const int fn = r.type == 1 ? Expression_GetFunctionForOp(r.data.cmdIdx) : r.data.cmdIdx;
        if ( fn >= 24 && fn - 24 < 457 && g_expFunctionNames[fn - 24] && !I_stricmp(g_expFunctionNames[fn - 24], name) )
            return true;
    }
    return false;
}

// an expression that is a constant (no dvars, no functions): its value; -1 if it isn't one ("1", "1 !")
static int Exp_Constant(const ExpressionStatement *exp)
{
    if ( !exp || !exp->filename || !exp->rpn )
        return 1; // (no expression: shown)
    // (numRpn counts the end too)
    const expressionRpn *r = exp->rpn;
    if ( exp->numRpn >= 2 && r[0].type == 0 && r[0].data.constant.dataType == VAL_INT )
    {
        if ( r[1].type == 3 )
            return r[0].data.constant.internals.intVal != 0;
        if ( exp->numRpn >= 3 && (r[1].type == 1 || r[1].type == 2) && r[2].type == 3 )
        {
            const int fn = r[1].type == 1 ? Expression_GetFunctionForOp(r[1].data.cmdIdx) : r[1].data.cmdIdx;
            if ( fn >= 0 && fn < 24 && g_expOperatorNames[fn] && !strcmp(g_expOperatorNames[fn], "!") )
                return r[0].data.constant.internals.intVal == 0;
        }
    }
    return -1;
}

// the action scripts of an item's event (action, onFocus, leaveFocus), joined
static std::string Item_EventText(const itemDef_s *item, const char *event)
{
    std::string text;
    for ( const GenericEventHandler *h = item->onEvent; h; h = h->next )
    {
        if ( !h->name || I_stricmp(h->name, event) )
            continue;
        for ( const GenericEventScript *s = h->eventScript; s; s = s->next )
        {
            if ( s->action )
                text += std::string(s->action) + " ; ";
        }
    }
    return text;
}

// the word after key in a script ("ui_highlight 3" -> 3, "ui_choicegroup \"level\"" -> level)
static std::string Script_Value(const std::string &script, const char *key)
{
    size_t p = script.find(key);
    if ( p == std::string::npos )
        return "";
    p += strlen(key);
    while ( p < script.size() && (script[p] == ' ' || script[p] == '"') )
        ++p;
    size_t e = p;
    while ( e < script.size() && script[e] != ' ' && script[e] != '"' && script[e] != ';' )
        ++e;
    return script.substr(p, e - p);
}

static std::string Color(const float *c)
{
    return Fmt("%g %g %g %g", c[0], c[1], c[2], c[3]);
}

// an item in the look of t at y: everything of an itemDef but its visibility, text and scripts
static std::string Item_Look(itemDef_s *t, float y, float h = -1.0f)
{
    const rectDef_s &r = t->window.rectClient;
    std::string s = Fmt("type %d rect %g %g %g %g %d %d style %d ", t->type, r.x, y, r.w, h < 0.0f ? r.h : h,
        r.horzAlign, r.vertAlign, t->window.style);
    if ( t->window.background )
        s += Fmt("background \"%s\" ", Material_GetName(t->window.background));
    s += Fmt("forecolor %s backcolor %s ", Color(t->window.foreColor).c_str(), Color(t->window.backColor).c_str());
    if ( t->window.border )
        s += Fmt("border %d bordersize %g bordercolor %s ", t->window.border, t->window.borderSize,
            Color(t->window.borderColor).c_str());
    if ( Item_IsTextDefType(t) && t->typeData.textDef )
    {
        const textDef_s *d = t->typeData.textDef;
        s += Fmt("textfont %d textscale %g textalign %d textalignx %g textaligny %g textstyle %d ", d->fontEnum,
            d->textscale, d->textAlignMode, d->textalignx, d->textaligny, d->textStyle);
    }
    if ( t->window.staticFlags & 0x800000 )
        s += "autowrapped ";
    if ( t->window.staticFlags & 0x100000 )
        s += "decoration ";
    return s;
}

// ----- the rows and the panel, as a text menu -----

// the map mods' rows (their first at flowY), our panel and the scroll bar
static std::string Levels_MenuText(itemDef_s *const *rowItems, int rowItemCount, itemDef_s *button, int highlight0,
    itemDef_s *title, itemDef_s *desc, itemDef_s *preview, float flowY, float top, float bottom)
{
    const std::string onFocus = Item_EventText(button, "onFocus");
    std::string group = Script_Value(onFocus, "ui_choicegroup");
    if ( group.empty() )
        group = "level";
    std::string over = Script_Value(onFocus, "play");
    if ( over.empty() )
        over = "uin_navigation_over";
    std::string click = Script_Value(Item_EventText(button, "action"), "play");
    if ( click.empty() )
        click = "uin_navigation_click";
    const float rowY = button->window.rectClient.y;
    std::string text = "menuDef { name \"bo1_levels_parts\" rect 0 0 640 480 0 0 ";
    int n = highlight0;
    const std::vector<LevelsRowSpec> specs = Levels_RowSpecs();
    for ( size_t k = 0; k < specs.size(); ++k )
    {
        const ModInfo &mod = *specs[k].mod;
        ++n;
        const std::string label = Quote(Upper(specs[k].label));
        const std::string shown = Fmt("dvarint( \"bo1_mods_%s\" ) == 1", mod.folder.c_str());
        for ( int i = 0; i < rowItemCount; ++i )
        {
            itemDef_s *t = rowItems[i];
            std::string visible;
            if ( t == button )
            {
                visible = shown;
            }
            else if ( Exp_HasString(&t->visibleExp, "ui_highlight") )
            {
                visible = Fmt("%s%s && localvarint( \"ui_highlight\" ) == %d && localvarstring( \"ui_choicegroup\" ) == \"%s\"",
                    Exp_HasFunction(&t->visibleExp, "menuhasfocus") ? "menuhasfocus() && " : "", shown.c_str(), n,
                    group.c_str());
            }
            else if ( Exp_Constant(&t->visibleExp) == 1 )
            {
                visible = shown;
            }
            else
            {
                continue; // (the row's other pictures: a map not found yet, ...)
            }
            text += Fmt("itemDef { name \"%s%d\" %s", Levels_RowPrefix((int)k).c_str(), i,
                Item_Look(t, t->window.rectClient.y - rowY + flowY).c_str());
            if ( Item_IsTextDefType(t) )
                text += Fmt("text \"%s\" ", label.c_str());
            text += Fmt("visible when ( %s ) ", visible.c_str());
            if ( t == button )
            {
                text += Fmt("onFocus { play \"%s\" ; setLocalVarInt ui_highlight %d ; setLocalVarString ui_choicegroup "
                            "\"%s\" ; } leaveFocus { setLocalVarInt ui_highlight 0 ; } ", over.c_str(), n, group.c_str());
                text += Fmt("action { play \"%s\" ; uiScript bo1LevelsPlay %s %s ; } ", click.c_str(), mod.folder.c_str(),
                    specs[k].map.c_str());
            }
            text += "} ";
        }
    }
    // the panel: the name and text where the game's are, a box saying it's a map mod where the picture is
    const std::string panel = "visible when ( dvarint( \"bo1_levels_panel\" ) == 1 ) ";
    if ( preview )
    {
        const rectDef_s &r = preview->window.rectClient;
        text += Fmt("itemDef { name \"bo1_levels_box\" type 1 rect %g %g %g %g %d %d style 1 backcolor 0 0 0 0.45 "
                    "border 1 bordersize 1 bordercolor 1 1 1 0.2 decoration %s} ", r.x, r.y, r.w, r.h, r.horzAlign,
            r.vertAlign, panel.c_str());
        text += Fmt("itemDef { name \"bo1_levels_boxtext\" type 1 rect %g %g %g %g %d %d textfont 1 textscale 0.4 "
                    "textalign 5 forecolor 1 1 1 0.6 text \"MAP MOD\" decoration %s} ", r.x, r.y + r.h * 0.5f - 12.0f,
            r.w, 24.0f, r.horzAlign, r.vertAlign, panel.c_str());
    }
    if ( title )
        text += Fmt("itemDef { name \"bo1_levels_title\" %s exp text ( dvarstring( \"bo1_levels_title\" ) ) %s} ",
            Item_Look(title, title->window.rectClient.y).c_str(), panel.c_str());
    if ( desc )
        text += Fmt("itemDef { name \"bo1_levels_desc\" %s exp text ( dvarstring( \"bo1_levels_desc\" ) ) %s} ",
            Item_Look(desc, desc->window.rectClient.y).c_str(), panel.c_str());
    // the scroll bar, right of the list
    const rectDef_s &b = button->window.rectClient;
    float right = b.x + b.w;
    for ( int i = 0; i < rowItemCount; ++i )
        right = std::max(right, rowItems[i]->window.rectClient.x + rowItems[i]->window.rectClient.w);
    text += UI_Scroll_BarText("bo1_levels", right + 5.0f, top, bottom, b.horzAlign, b.vertAlign, "bo1_levels_scroll");
    text += "} ";
    return text;
}

// ----- the panel -----

// the game's title, description and picture hidden while a map mod's row has the focus (and ours shown), back after
// (row: nullptr)
static void Levels_SetPanel(int contextIndex, const LevelsModRow *row)
{
    const bool mod = row != nullptr;
    const std::string key = row ? row->folder + " " + row->map : "";
    if ( mod != s_stockPanelHidden )
    {
        for ( LevelsPanelItem &p : s_stockPanel )
        {
            if ( mod )
            {
                p.wasVisible = (Window_GetDynamicFlags(contextIndex, &p.item->window) & 4) != 0;
                Window_RemoveDynamicFlags(contextIndex, &p.item->window, 4);
            }
            else if ( p.wasVisible )
            {
                Window_AddDynamicFlags(contextIndex, &p.item->window, 4);
            }
        }
        s_stockPanelHidden = mod;
        Dvar_SetInt((dvar_s *)bo1_levels_panel, mod);
    }
    if ( mod && key != s_panelRow )
    {
        const ModInfo *info = FindMod(row->folder);
        std::string desc;
        std::string title;
        if ( info )
        {
            title = Upper(row->label);
            desc = info->description.empty() ? "A map mod." : info->description;
            std::string why;
            if ( !Mods_IsAvailable(*info, &why) )
                desc = why;
            else if ( !Mods_RestartReason(*info).empty() )
                desc = "Restart the game first: " + row->label + " needs more memory set aside when the game "
                       "starts. " + desc;
        }
        Dvar_SetString((dvar_s *)bo1_levels_title, Quote(title).c_str());
        Dvar_SetString((dvar_s *)bo1_levels_desc, Quote(desc).c_str());
    }
    s_panelRow = key;
}

void UI_Levels_Frame(int localClientNum, UiContext *dc, menuDef_t *menu)
{
    if ( !s_menu || menu != s_menu || menu->items != s_items.data() )
        return;
    // which row has the focus: a map mod's (our panel), the game's (theirs), none (as it was)
    for ( const UiScrollRow &row : s_scroll.rows )
    {
        if ( !row.focus || !(Window_GetDynamicFlags(dc->contextIndex, &row.focus->window) & 2) )
            continue;
        const LevelsModRow *modRow = nullptr;
        for ( const LevelsModRow &r : s_rows )
        {
            if ( r.button == row.focus )
                modRow = &r;
        }
        Levels_SetPanel(dc->contextIndex, modRow);
        break;
    }
    (void)localClientNum;
}

// ----- loading -----

static void Levels_FreeItem(itemDef_s *item)
{
    Expression_Free(&item->visibleExp);
    Expression_Free(&item->forecolorAExp);
    if ( Item_IsTextDefType(item) && item->typeData.textDef && item->typeData.textDef->textExpData )
        Expression_Free(&item->typeData.textDef->textExpData->textExp);
}

void UI_Levels_Shutdown(UiContext *dc)
{
    UI_Scroll_Remove(&s_scroll);
    if ( !s_menu )
        return;
    // levels_zombie back as the game made it, if it is still loaded (a menu freed with its zone has left the list). Its
    // zone may stay loaded until the front end's next visit (a patch zone's menu): the game's list items go back where
    // they were, not where the list last scrolled them (or far below the screen), as the next visit measures them
    for ( int i = 0; i < dc->menuCount; ++i )
    {
        if ( dc->Menus[i] == s_menu && s_menu->items == s_items.data() )
        {
            Levels_SetPanel(dc->contextIndex, nullptr);
            s_menu->items = s_origItems;
            s_menu->itemCount = s_origItemCount;
            for ( const UiScrollRow &row : s_scroll.rows )
            {
                if ( row.flow )
                    continue; // (ours, freed below)
                for ( size_t n = 0; n < row.items.size(); ++n )
                    row.items[n]->window.rectClient.y = row.y + row.offsets[n];
            }
            Menu_UpdatePosition(dc->contextIndex, s_menu);
            Menu_SetCursorItem(dc->contextIndex, s_menu, -1);
        }
    }
    for ( itemDef_s *item : s_ourItems )
    {
        if ( g_editItem == item )
        {
            g_editingField = 0;
            g_editItem = 0;
        }
        Levels_FreeItem(item);
    }
    s_menu = nullptr;
    s_items.clear();
    s_origItems = nullptr;
    s_origItemCount = 0;
    s_ourItems.clear();
    s_scroll = UiScrollList();
    s_rows.clear();
    s_stockPanel.clear();
    s_stockPanelHidden = false;
    s_panelRow.clear();
}

void UI_Levels_LoadMenus(int localClientNum, UiContext *dc)
{
    menuDef_t *menu = Menus_FindByName(dc, "levels_zombie");
    if ( s_menu && menu == s_menu && menu->items == s_items.data() )
        return; // (patched already, the UI not shut down since)
    if ( s_menu )
        UI_Levels_Shutdown(dc);
    if ( !menu )
    {
        Com_PrintWarning(13, "mods: no levels_zombie menu: the map mods aren't in Mission Select\n");
        return;
    }
    bo1_levels_title = _Dvar_RegisterString("bo1_levels_title", "", 0, "mod: Mission Select's map mod name");
    bo1_levels_desc = _Dvar_RegisterString("bo1_levels_desc", "", 0, "mod: Mission Select's map mod text");
    bo1_levels_panel = _Dvar_RegisterInt("bo1_levels_panel", 0, 0, 1, 0, "mod: Mission Select shows a map mod");
    _Dvar_RegisterBool("bo1_levels_scroll", 0, 0, "mod: Mission Select's list scrolls");
    Dvar_SetInt((dvar_s *)bo1_levels_panel, 0);

    // the game's rows: buttons that set ui_load_level
    std::vector<itemDef_s *> buttons;
    int highlight0 = 0;
    for ( int i = 0; i < menu->itemCount; ++i )
    {
        itemDef_s *item = menu->items[i];
        if ( !item || item->type != 3 || Item_EventText(item, "action").find("ui_load_level") == std::string::npos )
            continue;
        buttons.push_back(item);
        highlight0 = std::max(highlight0, atoi(Script_Value(Item_EventText(item, "onFocus"), "ui_highlight").c_str()));
    }
    // the first row a map always has (shown, nothing to find first): the look of ours
    itemDef_s *tmpl = nullptr;
    float step = 1.0e9f;
    float top = 1.0e9f;
    float lastY = -1.0e9f;
    for ( itemDef_s *b : buttons )
    {
        const float y = b->window.rectClient.y;
        top = std::min(top, y);
        lastY = std::max(lastY, y);
        for ( itemDef_s *o : buttons )
        {
            const float d = o->window.rectClient.y - y;
            if ( d > 1.0f )
                step = std::min(step, d);
        }
        if ( Exp_Constant(&b->visibleExp) == 1 && (!tmpl || y < tmpl->window.rectClient.y) )
            tmpl = b;
    }
    if ( buttons.size() < 2 || !tmpl || step > 200.0f )
    {
        Com_PrintWarning(13, "mods: levels_zombie's map rows not as expected (%d): the map mods aren't in Mission Select\n",
            (int)buttons.size());
        return;
    }
    const rectDef_s &tr = tmpl->window.rectClient;
    // its row: the items at its place (bar, highlight, button, texts)
    std::vector<itemDef_s *> rowItems;
    for ( int i = 0; i < menu->itemCount; ++i )
    {
        itemDef_s *item = menu->items[i];
        if ( !item )
            continue;
        const rectDef_s &r = item->window.rectClient;
        if ( fabsf(r.y - tr.y) <= 2.0f && r.x >= tr.x - 2.0f && r.x < tr.x + tr.w && r.h <= step * 2.0f )
            rowItems.push_back(item);
    }
    float rowH = 0.0f;
    float colX0 = 1.0e9f, colX1 = -1.0e9f;
    for ( itemDef_s *item : rowItems )
    {
        rowH = std::max(rowH, item->window.rectClient.h);
        colX0 = std::min(colX0, item->window.rectClient.x);
        colX1 = std::max(colX1, item->window.rectClient.x + item->window.rectClient.w);
    }
    rowH = std::min(rowH, step);
    // the frame the list is in: the smallest larger item around its first row; the list stops a little above its bottom
    float bottom = -1.0e9f;
    float frameArea = 1.0e30f;
    for ( int i = 0; i < menu->itemCount; ++i )
    {
        if ( !menu->items[i] )
            continue;
        const rectDef_s &r = menu->items[i]->window.rectClient;
        const float area = r.w * r.h;
        if ( r.h > step * 4.0f && r.x <= tr.x + 1.0f && r.x + r.w >= tr.x + tr.w - 1.0f && r.y <= tr.y + 1.0f
             && r.y + r.h >= tr.y + tr.h && area < frameArea && r.w < 1000.0f )
        {
            frameArea = area;
            bottom = r.y + r.h - 8.0f;
        }
    }
    // the game's items in the list's column, from its first row down: they scroll (each its own row of the list)
    UiScrollList scroll;
    float flowY = lastY + step;
    int lastListIndex = -1;
    for ( int i = 0; i < menu->itemCount; ++i )
    {
        itemDef_s *item = menu->items[i];
        if ( !item || item->rectExpData )
            continue; // (one placed by expressions isn't moved: left where it is)
        const rectDef_s &r = item->window.rectClient;
        if ( r.x >= colX0 - 1.0f && r.x + r.w <= colX1 + 1.0f && r.y >= top - 1.0f && r.h <= step * 2.0f
             && (bottom < -1.0e8f || r.y < bottom) )
        {
            UiScrollRow row;
            row.items.push_back(item);
            row.offsets.push_back(0.0f);
            row.y = r.y;
            if ( std::find(buttons.begin(), buttons.end(), item) != buttons.end() )
                row.focus = item;
            scroll.rows.push_back(row);
            flowY = std::max(flowY, std::max(r.y + step, r.y + r.h + step - rowH));
            lastListIndex = i;
        }
    }
    if ( bottom < -1.0e8f )
        bottom = flowY - step + rowH + step * 6.0f;
    bottom = std::max(bottom, lastY + rowH);
    // the game's title, description and picture of the focused map
    itemDef_s *title = nullptr, *desc = nullptr, *preview = nullptr;
    std::vector<LevelsPanelItem> stockPanel;
    for ( int i = 0; i < menu->itemCount; ++i )
    {
        itemDef_s *item = menu->items[i];
        if ( !item )
            continue;
        bool panel = false;
        if ( Item_IsTextDefType(item) && item->typeData.textDef && item->typeData.textDef->textExpData
             && Exp_HasString(&item->typeData.textDef->textExpData->textExp, "ui_level_loc_text") )
        {
            panel = true;
            if ( Exp_HasString(&item->typeData.textDef->textExpData->textExp, "_DESC") )
                desc = desc ? desc : item;
            else
                title = title ? title : item;
        }
        if ( item->type == 2 && item->typeData.imageDef && Exp_HasString(&item->typeData.imageDef->materialExp, "ui_mapname") )
        {
            panel = true;
            preview = preview ? preview : item;
        }
        if ( panel )
            stockPanel.push_back({ item, true });
    }

    const std::string text = Levels_MenuText(rowItems.data(), (int)rowItems.size(), tmpl, highlight0, title, desc,
        preview, flowY, top, bottom);
    MenuList *parsed = UI_LoadMenusFromMemory("bo1_levels.menu", text.c_str(), 3);
    if ( !parsed || parsed->menuCount != 1 || !parsed->menus[0] )
    {
        Com_PrintWarning(13, "mods: Mission Select's map mod rows didn't load\n");
        return;
    }
    menuDef_t *parts = parsed->menus[0];

    // our items into levels_zombie: the rows after the game's last list item (the keys go on to them), the rest last
    std::vector<itemDef_s *> rowsOurs, restOurs;
    for ( int i = 0; i < parts->itemCount; ++i )
    {
        itemDef_s *item = parts->items[i];
        if ( !item )
            continue;
        s_ourItems.push_back(item);
        item->parent = menu;
        item->ui3dWindowId = tmpl->ui3dWindowId;
        const char *name = item->window.name ? (const char *)item->window.name : "";
        if ( !strcmp(name, "bo1_levels_track") )
            scroll.track = item;
        if ( !strcmp(name, "bo1_levels_thumb") )
            scroll.thumb = item;
        const bool row = !strncmp(name, "bo1_levels_r", 12) && isdigit((unsigned char)name[12]);
        (row ? rowsOurs : restOurs).push_back(item);
    }
    parts->itemCount = 0; // (its items are levels_zombie's now)
    s_origItems = menu->items;
    s_origItemCount = menu->itemCount;
    for ( int i = 0; i < menu->itemCount; ++i )
    {
        s_items.push_back(menu->items[i]);
        if ( i == lastListIndex )
            s_items.insert(s_items.end(), rowsOurs.begin(), rowsOurs.end());
    }
    s_items.insert(s_items.end(), restOurs.begin(), restOurs.end());
    // the map mods' rows (a row per map): after the game's, each shown while its mod is on
    const std::vector<LevelsRowSpec> specs = Levels_RowSpecs();
    for ( size_t k = 0; k < specs.size(); ++k )
    {
        UiScrollRow row;
        row.flow = true;
        row.shownDvar = "bo1_mods_" + specs[k].mod->folder;
        const std::string prefix = Levels_RowPrefix((int)k);
        for ( itemDef_s *item : rowsOurs )
        {
            if ( strncmp((const char *)item->window.name, prefix.c_str(), prefix.size()) )
                continue;
            row.items.push_back(item);
            row.offsets.push_back(item->window.rectClient.y - flowY);
            if ( item->type == 3 )
                row.focus = item;
        }
        if ( !row.focus )
            continue;
        s_rows.push_back({ specs[k].mod->folder, specs[k].map, specs[k].label, row.focus });
        scroll.rows.push_back(row);
    }
    scroll.menu = menu;
    scroll.top = top;
    scroll.bottom = bottom;
    scroll.step = step;
    scroll.rowH = rowH;
    scroll.flowY = flowY;
    scroll.scrollDvar = "bo1_levels_scroll";
    s_scroll = scroll;
    s_menu = menu;
    s_stockPanel = stockPanel;
    menu->items = s_items.data();
    menu->itemCount = (int)s_items.size();
    Menu_SetCursorItem(dc->contextIndex, menu, -1);
    UI_Scroll_Add(&s_scroll);
    UI_Scroll_Layout(dc->contextIndex, &s_scroll, true);
    Com_Printf(13, "mods: Mission Select: %d map rows, %d map mod rows (row step %g, list %g to %g)\n",
        (int)buttons.size(), (int)s_rows.size(), step, top, bottom);
    (void)localClientNum;
}

// ----- the rows' script -----

bool UI_Levels_MenuScript(int localClientNum, UiContext *dc, const char *name, const char **args)
{
    if ( I_stricmp(name, "bo1LevelsPlay") )
        return false;
    char folder[256] = "";
    if ( !String_Parse(args, folder, sizeof(folder)) )
        return true;
    // (the row's map: its mod's first if not named)
    char mapName[256] = "";
    String_Parse(args, mapName, sizeof(mapName));
    const ModInfo *mod = FindMod(folder);
    const LevelsModRow *row = nullptr;
    for ( const LevelsModRow &r : s_rows )
    {
        if ( !I_stricmp(r.folder.c_str(), folder) && (!row || !I_stricmp(r.map.c_str(), mapName)) )
            row = &r;
    }
    if ( !mod || !row || !mod->mapMod || mod->maps.empty() || !s_menu )
        return true;
    if ( !Mods_IsAvailable(*mod, nullptr) || !Mods_RestartReason(*mod).empty() )
        return true; // (the text under the box says why)
    // the game's rows' way (their action): ui_load_level, then the front end script's startlevel
    const std::string &map = row->map;
    Com_Printf(13, "mods: starting %s (mods/%s)\n", map.c_str(), mod->dir.c_str());
    const std::string script = Fmt("setDvar ui_mapname \"%s\" ; setDvar ui_load_level \"%s\" ; "
        "exec \"set arcademode_lives 2; set arcademode_combined_score 0\" ; close self ; open dummy_menu ; "
        "uiscript sendMenuNotify \"startlevel 0\" ;", map.c_str(), map.c_str());
    Item_RunScript(localClientNum, dc, row->button, script.c_str());
    return true;
}
