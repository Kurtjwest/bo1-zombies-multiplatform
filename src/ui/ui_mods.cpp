#include <universal/bo1_ptrcast.h>
#include "ui_mods.h"
#include "ui_main.h"
#include "ui_shared.h"
#include "ui_shared_obj.h"
#include "ui_scroll.h"
#include "ui_utils.h"
#include <universal/com_expressions.h>
#include <qcommon/common.h>
#include <qcommon/com_mods.h>
#include <universal/com_files.h>
#include <universal/dvar.h>
#include <universal/q_shared.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>

// mod: the main menu's Mods screen. main_text's Mods row (UI_Port_PatchMainMenu) opens bo1_mods: a row per mod in
// mods/ with a mod.txt or a map's zone (src/qcommon/com_mods.cpp reads them; more than kMaxRows: the list scrolls,
// ui_scroll.cpp), left / right turn the selected one off / on, Enter opens its page (bo1_mods_<folder>): what it does,
// On / Off, its options. A map mod that is on is a row of the zombies menu's Mission Select (ui_levels.cpp). The choices are archived dvars (bo1_mods_<folder>,
// each option's own dvar); Mods_ApplyForMap stacks the mods that are on when a game starts. The menus are text menus
// parsed when the front end's menus load, in the look of the co-op popups (ui_coop.cpp): a white bar and black text on
// the row with the focus (the game draws an item's text in its forecolor with the focus too: each text is two items,
// white without the highlight and black with it; a value's black copy shows a dvar the menu keeps up to date).

char g_modsMenuOpenScript[] = "play \"uin_navigation_click\" ; uiScript bo1ModsOpen ;";

static const float kWidth = 440.0f;
static const float kRowStep = 19.0f;
static const int kMaxRows = 12;  // mods the list shows at once (the popup's height); more scroll
static const int kMaxPages = 64; // mods listed, each with its page

static const dvar_t *bo1_mods_info;   // the focused row's text
static const dvar_t *bo1_mods_notice; // restart / command line
static std::string s_lastFolder;      // the list's row to focus when it opens again
static std::string s_focusFolder;     // the row with the focus: its mod and row (0: the list's, 1: On / Off, 2..: options)
static int s_focusRow;
static UiScrollList s_listScroll;     // the list's rows, when there are more than kMaxRows (ui_scroll.cpp)

static std::string Fmt(const char *format, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

// text from a mod.txt in a quoted menu string: no quotes or backslashes, not a localization reference
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

// a page's first option row: its rows are ENABLED, then its options
static int OptionRow0(const ModInfo &)
{
    return 2;
}

static std::string MapLabel(const ModInfo &mod)
{
    return mod.maps.empty() ? mod.name : mod.maps[0].label;
}

// ----- the menus' text -----

struct ModsPopup
{
    std::string text;
    std::string group; // ui_choicegroup of its rows
    float w, h;
};

static void Pop_Begin(ModsPopup &p, const std::string &name, float h, const std::string &onOpen, const std::string &onEsc)
{
    p.w = kWidth;
    p.h = h;
    p.group = name;
    p.text += Fmt("menuDef { name \"%s\" rect %g %g %g %g 2 2 popup priority 0 focuscolor 1 1 1 1 ", name.c_str(),
        -p.w * 0.5f, -h * 0.5f, p.w, h);
    p.text += Fmt("onOpen { activateBlur ; %s } onClose { deactivateBlur ; } onESC { %s } ", onOpen.c_str(), onEsc.c_str());
    p.text += "itemDef { rect -2000 -2000 4000 4000 2 2 style 1 backcolor 0 0 0 0.7 decoration visible 1 } ";
    p.text += Fmt("itemDef { rect 0 0 %g %g 2 2 style 10 frame 16 0.5 background \"menu_mp_lobby_frame_circle\" "
                  "forecolor 0.2 0.2 0.2 0.6 decoration visible 1 } ", p.w, h);
    p.text += Fmt("itemDef { rect 0 0 %g %g 2 2 style 3 background \"menu_mp_lobby_frame_fade\" forecolor 1 1 1 0.15 "
                  "decoration visible 1 } ", p.w, h - 2.0f);
}

static void Pop_End(ModsPopup &p)
{
    p.text += "} ";
}

// a wrapped text at y: text, or the dvar named
static void Pop_Text(ModsPopup &p, float y, float h, const std::string &text, const char *dvar, float scale = 0.28f,
    const char *color = "1 1 1 1")
{
    p.text += Fmt("itemDef { type 1 rect 15 %g %g %g 2 2 textfont 1 textscale %g textalign 4 forecolor %s "
                  "decoration autowrapped visible 1 ", y, p.w - 30.0f, h, scale, color);
    if ( dvar )
        p.text += Fmt("exp text ( dvarstring( \"%s\" ) ) } ", dvar);
    else
        p.text += Fmt("text \"%s\" } ", Quote(text).c_str());
}

static std::string Pop_Highlighted(const ModsPopup &p, int n)
{
    return Fmt("( localvarint( \"ui_highlight\" ) == %d && localvarstring( \"ui_choicegroup\" ) == \"%s\" )", n,
        p.group.c_str());
}

// row n's bar at y, and its highlight while it has the focus (w: the row's width, 0 the popup's; name: its items')
static void Pop_RowBack(ModsPopup &p, float y, int n, float w = 0.0f, const std::string &name = "")
{
    w = w > 0.0f ? w : p.w - 30.0f;
    const std::string named = name.empty() ? "" : "name \"" + name + "\" ";
    p.text += Fmt("itemDef { %srect 15 %g %g 17 2 2 style 3 background \"white\" forecolor 0 0 0 0.3 decoration "
                  "visible 1 } ", named.c_str(), y, w);
    p.text += Fmt("itemDef { %srect 15 %g %g 17 2 2 style 3 background \"white\" forecolor 1 1 1 1 decoration "
                  "visible when %s } ", named.c_str(), y, w, Pop_Highlighted(p, n).c_str());
}

// a text on row n (x, w within the row): white, black on the highlight; text, or the dvar named
static void Pop_RowText(ModsPopup &p, float x, float y, float w, const std::string &text, const char *dvar, int n,
    const char *color = "1 1 1 1", const std::string &name = "")
{
    const std::string named = name.empty() ? "" : "name \"" + name + "\" ";
    for ( int black = 0; black < 2; ++black )
    {
        p.text += Fmt("itemDef { %stype 1 rect %g %g %g 17 2 2 textfont 1 textscale 0.315 textalign 8 textalignx 17 "
                      "forecolor %s decoration visible when ( %s%s ) ", named.c_str(), x, y, w, black ? "0 0 0 1" : color,
                      black ? "" : "!", Pop_Highlighted(p, n).c_str());
        if ( dvar )
            p.text += Fmt("exp text ( dvarstring( \"%s\" ) ) } ", dvar);
        else
            p.text += Fmt("text \"%s\" } ", Quote(text).c_str());
    }
}

// what row n runs when it gets and loses the focus
static std::string Pop_RowFocus(const ModsPopup &p, int n, const std::string &onFocus)
{
    return Fmt("onFocus { play \"uin_navigation_submenu_over\" ; setLocalVarInt ui_highlight %d ; "
               "setLocalVarString ui_choicegroup \"%s\" ; %s } leaveFocus { setLocalVarInt ui_highlight 0 ; } ",
        n, p.group.c_str(), onFocus.c_str());
}

// a button on row n (DONE, BACK): its own text clear, its label white / black
static void Pop_Button(ModsPopup &p, const std::string &name, float y, int n, const char *label, const std::string &action)
{
    Pop_RowBack(p, y, n);
    p.text += Fmt("itemDef { name \"%s\" type 3 rect 15 %g %g 17 2 2 textfont 1 textscale 0.315 textalign 8 "
                  "textalignx 17 forecolor 1 1 1 0 text \"%s\" visible 1 %s", name.c_str(), y, p.w - 30.0f, label,
                  Pop_RowFocus(p, n, "uiScript bo1ModsFocus - 0 ;").c_str());
    p.text += Fmt("action { play \"uin_navigation_click\" ; %s } } ", action.c_str());
    Pop_RowText(p, 15.0f, y, p.w - 30.0f, label, nullptr, n);
}

// the keys that turn a list row's mod off (left) and on (right): arrows, d-pad, left stick, keypad
static const int kLeftKeys[] = { 156, 22, 30, 164 };
static const int kRightKeys[] = { 157, 23, 31, 163 };

static float List_RowsY()
{
    return 72.0f;
}

// the mods listed (at most kMaxPages), and the rows shown at once (more: the list scrolls)
static int List_Count()
{
    return std::min((int)Mods_List().size(), kMaxPages);
}

static int List_Rows()
{
    return std::min(List_Count(), kMaxRows);
}

static std::string List_MenuText()
{
    const std::vector<ModInfo> &mods = Mods_List();
    const int count = List_Count();
    const int rows = List_Rows();
    const bool scrolls = count > rows;
    const float rowsY = List_RowsY();
    const float infoY = rowsY + rows * kRowStep + 6.0f;
    const float noticeY = infoY + 58.0f;
    const float doneY = noticeY + 32.0f;
    ModsPopup p;
    Pop_Begin(p, "bo1_mods", doneY + 17.0f + 15.0f, "uiScript bo1ModsOpened ;", "close self ;");
    Pop_Text(p, 15.0f, 20.0f, "MODS", nullptr, 0.315f);
    Pop_Text(p, 37.0f, 32.0f,
        "Mods change the zombies games you start from the menus: Solo, Host Game, Join Game; a map mod that is on is in "
        "Mission Select. Left and right turn the selected mod off and on; Enter shows what it does and its settings.",
        nullptr, 0.26f, "0.85 0.85 0.85 1");
    // (a scroll bar at the right of the rows when they don't all fit)
    const float w = p.w - 30.0f - (scrolls ? 12.0f : 0.0f);
    for ( int i = 0; i < count; ++i )
    {
        const ModInfo &mod = mods[i];
        const float y = rowsY + i * kRowStep;
        const int n = i + 1;
        const std::string part = Fmt("bo1_mods_rowpart_%d", i);
        Pop_RowBack(p, y, n, w, part);
        p.text += Fmt("itemDef { name \"bo1_mods_row_%s\" type 3 rect 15 %g %g 17 2 2 textfont 1 textscale 0.315 "
                      "textalign 8 textalignx 17 forecolor 1 1 1 0 text \"%s\" visible 1 %s", mod.folder.c_str(), y, w,
                      Quote(Upper(mod.name)).c_str(),
                      Pop_RowFocus(p, n, Fmt("uiScript bo1ModsFocus %s 0 ;", mod.folder.c_str())).c_str());
        p.text += Fmt("action { play \"uin_navigation_click\" ; close self ; open \"bo1_mods_%s\" ; } ", mod.folder.c_str());
        for ( int on = 0; on < 2; ++on )
        {
            for ( int key : on ? kRightKeys : kLeftKeys )
                p.text += Fmt("execKeyInt %d { play \"uin_navigation_click\" ; uiScript bo1ModsSet %s %d ; } ", key,
                    mod.folder.c_str(), on);
        }
        p.text += "} ";
        // its name, its state at the right
        Pop_RowText(p, 15.0f, y, w - 150.0f, Upper(mod.name), nullptr, n, "1 1 1 1", part);
        Pop_RowText(p, 15.0f + w - 150.0f, y, 150.0f, "", Fmt("bo1_mods_status_%s", mod.folder.c_str()).c_str(), n,
            "1 1 1 1", part);
    }
    if ( scrolls )
        p.text += UI_Scroll_BarText("bo1_mods", 15.0f + (p.w - 30.0f) - 6.0f, rowsY, rowsY + rows * kRowStep - 2.0f, 2,
            2, "bo1_mods_scroll");
    Pop_Text(p, infoY, 56.0f, "", "bo1_mods_info", 0.26f, "0.85 0.85 0.85 1");
    Pop_Text(p, noticeY, 30.0f, "", "bo1_mods_notice", 0.26f, "1 0.75 0.4 1");
    Pop_Button(p, "bo1_mods_done", doneY, count + 1, "DONE", "close self ;");
    Pop_End(p);
    return p.text;
}

static std::string Page_MenuText(const ModInfo &mod)
{
    const std::string name = "bo1_mods_" + mod.folder;
    const int rows = OptionRow0(mod) - 1 + (int)mod.options.size();
    const float rowsY = 106.0f;
    const float infoY = rowsY + rows * kRowStep + 6.0f;
    const float noticeY = infoY + 46.0f;
    const float backY = noticeY + 32.0f;
    ModsPopup p;
    Pop_Begin(p, name, backY + 17.0f + 15.0f, Fmt("uiScript bo1ModsPageOpened %s ;", mod.folder.c_str()),
        "close self ; open bo1_mods ;");
    Pop_Text(p, 15.0f, 20.0f, Upper(mod.name), nullptr, 0.315f);
    Pop_Text(p, 37.0f, 46.0f, mod.description, nullptr, 0.26f, "0.85 0.85 0.85 1");
    // where it plays, what it needs
    std::string where = "Plays on " + Mods_MapList(mod) + ".";
    if ( mod.mapMod )
        where = "A map: " + MapLabel(mod) + (mod.maps.empty() ? "" : " (" + mod.maps[0].name + ")")
                + (mod.version.empty() ? "" : ", version " + mod.version) + ".";
    std::string why;
    if ( !Mods_IsAvailable(mod, &why) )
        where += " " + why;
    Pop_Text(p, 85.0f, 16.0f, where, nullptr, 0.26f, "0.6 0.8 1 1");
    const float w = p.w - 30.0f;
    for ( int k = 1; k <= rows; ++k )
    {
        const float y = rowsY + (k - 1) * kRowStep;
        const ModOption *o = k >= OptionRow0(mod) ? &mod.options[k - OptionRow0(mod)] : nullptr;
        Pop_RowBack(p, y, k);
        Pop_RowText(p, 15.0f, y, w * 0.6f, o ? Upper(o->label) : "ENABLED", nullptr, k);
        // the value: the row's focus item, at the right (white; black on the highlight is the copy below)
        p.text += Fmt("itemDef { name \"%s_%d\" type 10 rect 15 %g %g 17 2 2 textfont 1 textscale 0.315 textalign 10 "
                      "textalignx -12 forecolor 1 1 1 1 dvar \"%s\" dvarStrList { ", name.c_str(), k, y, w,
                      o ? o->dvar.c_str() : ("bo1_mods_" + mod.folder).c_str());
        // (labels and values with ';' between them: the menu parser joins strings next to each other into one)
        if ( o )
        {
            for ( const ModValue &v : o->values )
                p.text += Fmt("\"%s\" ; \"%s\" ; ", Quote(v.label).c_str(), Quote(v.value).c_str());
        }
        else
        {
            p.text += "\"OFF\" ; \"0\" ; \"ON\" ; \"1\" ; ";
        }
        p.text += Fmt("} visible 1 %s", Pop_RowFocus(p, k, Fmt("uiScript bo1ModsFocus %s %d ;", mod.folder.c_str(), k)).c_str());
        p.text += "action { play \"uin_navigation_click\" ; uiScript bo1ModsChanged ; } ";
        // (the gamepad's A: the next value, as Enter)
        p.text += Fmt("execKeyInt 1 { play \"uin_navigation_click\" ; uiScript bo1ModsNext %s %d ; } } ",
            mod.folder.c_str(), k);
        // the value in black on the highlight, where the item draws it (a type 1 item showing a dvar: placed again as
        // its width changes, as the value's own)
        p.text += Fmt("itemDef { type 1 rect 15 %g %g 17 2 2 textfont 1 textscale 0.315 textalign 10 textalignx -12 "
                      "forecolor 0 0 0 1 dvar \"bo1_mods_val_%s_%d\" decoration visible when %s } ", y, w,
                      mod.folder.c_str(), k, Pop_Highlighted(p, k).c_str());
    }
    Pop_Text(p, infoY, 44.0f, "", "bo1_mods_info", 0.26f, "0.85 0.85 0.85 1");
    Pop_Text(p, noticeY, 30.0f, "", "bo1_mods_notice", 0.26f, "1 0.75 0.4 1");
    Pop_Button(p, name + "_back", backY, rows + 1, "BACK", "close self ; open bo1_mods ;");
    Pop_End(p);
    return p.text;
}

// ----- what the menus show -----

static std::string Mods_Status(const ModInfo &mod)
{
    if ( !Mods_IsAvailable(mod, nullptr) )
        return "UNAVAILABLE";
    if ( !Mods_IsEnabled(mod) )
        return "OFF";
    return Mods_RestartReason(mod).empty() ? "ON" : "ON - RESTART";
}

// the text under the rows for the focused row
static std::string Mods_Info(const ModInfo *mod, int row)
{
    if ( !mod )
        return Mods_List().empty() ? "No mods found: a mod is a folder in the game folder's mods folder with a mod.txt "
                                     "(README.md, Mods menu)."
                                   : "";
    std::string why;
    const bool available = Mods_IsAvailable(*mod, &why);
    const std::string restart = Mods_RestartReason(*mod);
    if ( row == 0 || row == 1 )
    {
        std::string text = row == 0 ? (mod->mapMod && mod->description.empty() ? "A map."
                                       : mod->zoneMod && mod->description.empty() ? "A mod-tools mod."
                                                                                   : mod->description)
                         : mod->mapMod ? "A map: " + MapLabel(*mod) + "."
                                       : "Plays on " + Mods_MapList(*mod) + ".";
        if ( row == 1 && mod->zoneMod )
            text += " One such mod at a time (each has its own mod.ff), not on map mods.";
        if ( !available )
            text += " " + why;
        else if ( Mods_IsEnabled(*mod) && !restart.empty() )
            text += " Restart the game to finish turning it on: it needs memory set aside when the game starts.";
        else if ( row == 1 && mod->mapMod )
            text += Mods_IsEnabled(*mod) ? " On: it's in the zombies menu's Mission Select (Solo, Host Game)."
                                         : " Off: it isn't in Mission Select.";
        else if ( row == 1 )
            text += Mods_IsEnabled(*mod) ? " On: it's in the next game you start." : " Off: games start without it.";
        return text;
    }
    if ( row - OptionRow0(*mod) >= (int)mod->options.size() )
        return "";
    const ModOption &o = mod->options[row - OptionRow0(*mod)];
    std::string text = o.help;
    const int i = Mods_OptionValueIndex(o);
    if ( i >= 0 && Mods_IsEnabled(*mod) && restart.find(o.label + ": " + o.values[i].label) != std::string::npos )
        text += (text.empty() ? "" : " ") + std::string("Restart the game to use ") + o.values[i].label + ".";
    return text;
}

// after the player changed a mod: one that can't play (its zone files aren't installed) stays off; a zone mod turned on
// (changed) turns the others off (a game loads one mod.ff)
static void Mods_KeepOff(const char *changed = nullptr)
{
    const ModInfo *on = changed ? FindMod(changed) : nullptr;
    if ( on && !(on->zoneMod && Mods_IsEnabled(*on)) )
        on = nullptr;
    for ( const ModInfo &mod : Mods_List() )
    {
        if ( Mods_IsEnabled(mod) && (!Mods_IsAvailable(mod, nullptr) || (on && mod.zoneMod && &mod != on)) )
            Dvar_SetBool((dvar_s *)mod.enabled, false);
    }
}

static void Mods_Refresh()
{
    static std::set<std::string> names; // (dvars keep their name's pointer)
    for ( const ModInfo &mod : Mods_List() )
    {
        const char *name = names.insert("bo1_mods_status_" + mod.folder).first->c_str();
        const dvar_t *status = _Dvar_RegisterString(name, "", 0, "mod: the Mods menu's state of a mod");
        Dvar_SetString((dvar_s *)status, Mods_Status(mod).c_str());
        // each page row's value as its list shows it (the row's black copy on the highlight)
        for ( int k = 1; k < OptionRow0(mod) + (int)mod.options.size(); ++k )
        {
            std::string label;
            if ( k < OptionRow0(mod) )
            {
                label = Mods_IsEnabled(mod) ? "ON" : "OFF";
            }
            else
            {
                const ModOption &o = mod.options[k - OptionRow0(mod)];
                const int i = Mods_OptionValueIndex(o);
                label = i >= 0 ? Quote(o.values[i].label) : "";
            }
            name = names.insert(Fmt("bo1_mods_val_%s_%d", mod.folder.c_str(), k)).first->c_str();
            const dvar_t *value = _Dvar_RegisterString(name, "", 0, "mod: a value on the Mods menu's page of a mod");
            Dvar_SetString((dvar_s *)value, label.c_str());
        }
    }
    std::string who;
    const bool restart = Mods_SaveStartup(&who);
    std::string notice;
    if ( Com_ModStartup_CommandLineMods() )
        notice = Fmt("This game was started with mods on its command line (fs_mods \"%s\"): the choices here are used "
                     "when it starts without them.", fs_modsVar ? (const char *)fs_modsVar->current.string : "");
    else if ( restart )
        notice = "Restart the game to finish setting up " + who + " (memory set aside when the game starts).";
    Dvar_SetString((dvar_s *)bo1_mods_notice, notice.c_str());
    Dvar_SetString((dvar_s *)bo1_mods_info, Mods_Info(FindMod(s_focusFolder), s_focusRow).c_str());
}

// ----- loading -----

// the menus parsed into the hunk (with the front end's other menus, which a level's load clears) and their expressions
// into Z memory, freed at UI_Shutdown
static menuDef_t *s_modsMenus[1 + kMaxPages];
static int s_modsMenuCount;

static void UI_Mods_FreeMenu(menuDef_t *menu)
{
    for ( int i = 0; i < menu->itemCount; ++i )
    {
        itemDef_s *item = menu->items[i];
        if ( !item )
            continue;
        Expression_Free(&item->visibleExp);
        Expression_Free(&item->forecolorAExp);
        if ( Item_IsTextDefType(item) && item->typeData.textDef && item->typeData.textDef->textExpData )
            Expression_Free(&item->typeData.textDef->textExpData->textExp);
    }
    Expression_Free(&menu->visibleExp);
}

// the list's rows scroll when there are more than kMaxRows: each row's items (its bar, button and texts), at its place
static void UI_Mods_SetUpListScroll(UiContext *dc)
{
    UI_Scroll_Remove(&s_listScroll);
    s_listScroll = UiScrollList();
    menuDef_t *menu = Menus_FindByName(dc, "bo1_mods");
    if ( !menu || List_Count() <= List_Rows() )
        return;
    _Dvar_RegisterBool("bo1_mods_scroll", 0, 0, "mod: the Mods menu's list scrolls");
    UiScrollList &list = s_listScroll;
    list.menu = menu;
    list.top = List_RowsY();
    list.bottom = List_RowsY() + List_Rows() * kRowStep - 2.0f;
    list.step = kRowStep;
    list.rowH = 17.0f;
    list.flowY = list.top;
    list.scrollDvar = "bo1_mods_scroll";
    list.rows.resize(List_Count());
    for ( int i = 0; i < List_Count(); ++i )
        list.rows[i].y = List_RowsY() + i * kRowStep;
    for ( int k = 0; k < menu->itemCount; ++k )
    {
        itemDef_s *item = menu->items[k];
        const char *name = item && item->window.name ? (const char *)item->window.name : "";
        int row = -1;
        if ( !strncmp(name, "bo1_mods_rowpart_", 17) )
        {
            row = atoi(name + 17);
        }
        else if ( !strncmp(name, "bo1_mods_row_", 13) )
        {
            for ( int i = 0; i < List_Count(); ++i )
                row = !I_stricmp(name + 13, Mods_List()[i].folder.c_str()) ? i : row;
            if ( row >= 0 )
                list.rows[row].focus = item;
        }
        else if ( !strcmp(name, "bo1_mods_track") )
        {
            list.track = item;
        }
        else if ( !strcmp(name, "bo1_mods_thumb") )
        {
            list.thumb = item;
        }
        if ( row < 0 || row >= List_Count() )
            continue;
        list.rows[row].items.push_back(item);
        list.rows[row].offsets.push_back(0.0f);
    }
    UI_Scroll_Add(&s_listScroll);
    UI_Scroll_Layout(dc->contextIndex, &s_listScroll, true);
}

void UI_Mods_LoadMenus(int localClientNum, UiContext *dc)
{
    Mods_RegisterCommands();
    Mods_SetMenuActive();
    if ( Menus_FindByName(dc, "bo1_mods") )
    {
        Mods_Refresh(); // (loaded already, the UI not shut down since)
        return;
    }
    bo1_mods_info = _Dvar_RegisterString("bo1_mods_info", "", 0, "mod: the Mods menu's text for the focused row");
    bo1_mods_notice = _Dvar_RegisterString("bo1_mods_notice", "", 0, "mod: the Mods menu's restart notice");
    Mods_Discover();
    s_modsMenuCount = 0; // (any earlier ones went with the UI's last shutdown)
    std::string text = List_MenuText();
    for ( int i = 0; i < List_Count(); ++i )
        text += Page_MenuText(Mods_List()[i]);
    MenuList *list = UI_LoadMenusFromMemory("bo1_mods.menu", text.c_str(), 3);
    for ( int i = 0; i < list->menuCount; ++i )
    {
        menuDef_t *menu = list->menus[i];
        if ( Menus_FindByName(dc, menu->window.name) || dc->menuCount >= 600 || s_modsMenuCount >= 1 + kMaxPages )
        {
            UI_Mods_FreeMenu(menu);
            continue;
        }
        // (UI_AddMenu, without its test that a menu is a fast-file asset)
        dc->Menus[dc->menuCount++] = menu;
        s_modsMenus[s_modsMenuCount++] = menu;
        Menus_Close(localClientNum, dc, menu);
    }
    UI_Mods_SetUpListScroll(dc);
    Com_Printf(13, "mods: Mods menu with %d mods (%d menus)\n", (int)Mods_List().size(), s_modsMenuCount);
    s_focusFolder.clear();
    s_focusRow = 0;
    Mods_Refresh();
}

void UI_Mods_Shutdown(UiContext *dc)
{
    UI_Scroll_Remove(&s_listScroll);
    s_listScroll = UiScrollList();
    int kept = 0;
    for ( int i = 0; i < dc->menuCount; ++i )
    {
        bool ours = false;
        for ( int k = 0; k < s_modsMenuCount; ++k )
            ours = ours || dc->Menus[i] == s_modsMenus[k];
        if ( !ours )
            dc->Menus[kept++] = dc->Menus[i];
    }
    dc->menuCount = kept;
    for ( int i = 0; i < s_modsMenuCount; ++i )
    {
        if ( g_editItem && g_editItem->parent == s_modsMenus[i] )
        {
            g_editingField = 0;
            g_editItem = 0;
        }
        UI_Mods_FreeMenu(s_modsMenus[i]);
    }
    s_modsMenuCount = 0;
}

// ----- the menus' scripts -----

static void Mods_Focus(int localClientNum, UiContext *dc, const char *menuName, const std::string &itemName)
{
    menuDef_t *menu = Menus_FindByName(dc, menuName);
    if ( menu && menu->itemCount && Menus_MenuIsInStack(dc, menu) )
        Item_RunScript(localClientNum, dc, menu->items[0], ("setfocus \"" + itemName + "\"").c_str());
}

// the next value of a page's row (the gamepad's A): On / Off, or the option's next value
static void Mods_Next(const ModInfo &mod, int row)
{
    if ( row == 1 )
    {
        Dvar_SetBool((dvar_s *)mod.enabled, !Mods_IsEnabled(mod));
        return;
    }
    const int i0 = row - OptionRow0(mod);
    if ( i0 < 0 || i0 >= (int)mod.options.size() )
        return;
    const ModOption &o = mod.options[i0];
    const int i = Mods_OptionValueIndex(o);
    const std::string &next = o.values[(i + 1) % o.values.size()].value;
    Dvar_SetFromStringByName(o.dvar.c_str(), (char *)next.c_str());
}

bool UI_Mods_MenuScript(int localClientNum, UiContext *dc, const char *name, const char **args)
{
    if ( I_strnicmp(name, "bo1Mods", 7) )
        return false;
    char arg1[256] = "", arg2[64] = "";
    if ( !I_stricmp(name, "bo1ModsOpen") )
    {
        // (main_text's Mods row)
        s_lastFolder.clear();
        Menus_OpenByName(localClientNum, dc, "bo1_mods");
    }
    else if ( !I_stricmp(name, "bo1ModsOpened") )
    {
        Mods_Refresh();
        const ModInfo *mod = FindMod(s_lastFolder);
        if ( !mod && !Mods_List().empty() )
            mod = &Mods_List()[0];
        Mods_Focus(localClientNum, dc, "bo1_mods", mod ? "bo1_mods_row_" + mod->folder : "bo1_mods_done");
    }
    else if ( !I_stricmp(name, "bo1ModsPageOpened") && String_Parse(args, arg1, sizeof(arg1)) )
    {
        s_lastFolder = arg1;
        Mods_Refresh();
        Mods_Focus(localClientNum, dc, ("bo1_mods_" + s_lastFolder).c_str(), "bo1_mods_" + s_lastFolder + "_1");
    }
    else if ( !I_stricmp(name, "bo1ModsFocus") && String_Parse(args, arg1, sizeof(arg1))
              && String_Parse(args, arg2, sizeof(arg2)) )
    {
        s_focusFolder = arg1;
        s_focusRow = atoi(arg2);
        if ( s_focusRow == 0 && FindMod(arg1) )
            s_lastFolder = arg1;
        Dvar_SetString((dvar_s *)bo1_mods_info, Mods_Info(FindMod(s_focusFolder), s_focusRow).c_str());
    }
    else if ( !I_stricmp(name, "bo1ModsSet") && String_Parse(args, arg1, sizeof(arg1))
              && String_Parse(args, arg2, sizeof(arg2)) )
    {
        if ( const ModInfo *mod = FindMod(arg1) )
            Dvar_SetBool((dvar_s *)mod->enabled, atoi(arg2) != 0);
        Mods_KeepOff(arg1);
        Mods_Refresh();
    }
    else if ( !I_stricmp(name, "bo1ModsNext") && String_Parse(args, arg1, sizeof(arg1))
              && String_Parse(args, arg2, sizeof(arg2)) )
    {
        if ( const ModInfo *mod = FindMod(arg1) )
            Mods_Next(*mod, atoi(arg2));
        Mods_KeepOff(arg1);
        Mods_Refresh();
    }
    else if ( !I_stricmp(name, "bo1ModsChanged") )
    {
        Mods_KeepOff();
        Mods_Refresh();
    }
    else
    {
        Com_PrintWarning(13, "mods: unknown menu script %s\n", name);
    }
    return true;
}
