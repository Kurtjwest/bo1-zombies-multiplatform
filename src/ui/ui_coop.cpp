#include <universal/bo1_ptrcast.h>
#include "ui_coop.h"
#include "ui_main.h"
#include "ui_shared.h"
#include "ui_shared_obj.h"
#include "ui_utils.h"
#include <universal/com_expressions.h>
#include <qcommon/cmd.h>
#include <qcommon/common.h>
#include <universal/dvar.h>
#include <universal/q_shared.h>
#include <win32/win_net.h>
#include <cstdarg>
#include <cstdio>
#include <string>

// co-op: the zombies menu's Online choice. The SP front end's TV menu (main_online) offers Online (the retail
// game's online lobby, which needs its online service) and Solo. Online opens this port's popups instead: Host Game
// (how many players, then Solo's mission select: the level waits for them, g_scr_sp_players.cpp) and Join Game (the
// host's address). The popups are menus in the text format of the game's .menu files, parsed when the front end's
// menus load (UI_LoadMenusFromMemory), in the look of the front end's own popups (error_netconnect_popmenu).

static const dvar_t *bo1_coop_join_address; // the address typed in Join Game (kept in the config)
static const dvar_t *bo1_coop_host_info;    // Host Game's text (this computer's address)
static const dvar_t *bo1_coop_join_info;    // Join Game's text

// (va's few buffers would be reused while a menu's text is put together)
static std::string Fmt(const char *format, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

// a popup's menuDef: its frame, title and text, rows of buttons at the bottom
struct CoopPopup
{
    std::string text;
    float w, h;
    int buttons;
};

static void Popup_Begin(CoopPopup &p, const char *name, float w, float h, const char *onOpen, const char *onEsc)
{
    p.w = w;
    p.h = h;
    p.buttons = 0;
    p.text += Fmt("menuDef { name \"%s\" rect %g %g %g %g 2 2 popup priority 0 focuscolor 1 1 1 1 ", name, -w * 0.5f, -h * 0.5f, w, h);
    p.text += Fmt("onOpen { activateBlur ; %s } onClose { deactivateBlur ; } onESC { %s } ", onOpen, onEsc);
    // the screen behind darkened, the frame, its sheen
    p.text += "itemDef { rect -2000 -2000 4000 4000 2 2 style 1 backcolor 0 0 0 0.7 decoration visible 1 } ";
    p.text += Fmt("itemDef { rect 0 0 %g %g 2 2 style 10 frame 16 0.5 background \"menu_mp_lobby_frame_circle\" "
                 "forecolor 0.2 0.2 0.2 0.6 decoration visible 1 } ", w, h);
    p.text += Fmt("itemDef { rect 0 0 %g %g 2 2 style 3 background \"menu_mp_lobby_frame_fade\" forecolor 1 1 1 0.15 "
                 "decoration visible 1 } ", w, h - 2.0f);
}

// a text: dvar names the dvar it shows, or it shows text
static void Popup_Text(CoopPopup &p, float y, float h, const char *text, const char *dvar,
    const char *color = "1 1 1 1")
{
    p.text += Fmt("itemDef { type 1 rect 15 %g %g %g 2 2 textfont 1 textscale 0.315 textalign 4 forecolor %s "
                 "decoration autowrapped visible 1 ", y, p.w - 30.0f, h, color);
    if ( dvar )
        p.text += Fmt("exp text ( dvarstring( \"%s\" ) ) } ", dvar);
    else
        p.text += Fmt("text \"%s\" } ", text);
}

// a button on row y (the item named name), with its bar and highlight as the front end's popups draw them
static void Popup_Button(CoopPopup &p, const char *name, float y, const char *label, const std::string &action)
{
    const int n = ++p.buttons;
    const float w = p.w - 30.0f;
    const std::string highlighted = Fmt("( localvarint( \"ui_highlight\" ) == %d && localvarstring( \"ui_choicegroup\" ) == "
                                 "\"bo1_coop\" )", n);
    p.text += Fmt("itemDef { rect 15 %g %g 17 2 2 style 3 background \"white\" forecolor 0 0 0 0.3 decoration "
                 "visible 1 } ", y, w);
    p.text += Fmt("itemDef { rect 15 %g %g 17 2 2 style 3 background \"white\" forecolor 1 1 1 1 decoration "
                 "visible when %s } ", y, w, highlighted.c_str());
    p.text += Fmt("itemDef { name \"%s\" type 3 rect 15 %g %g 17 2 2 textfont 1 textscale 0.315 textalign 8 "
                 "textalignx 17 forecolor 1 1 1 1 text \"%s\" visible 1 ", name, y, w, label);
    p.text += Fmt("onFocus { play \"uin_navigation_submenu_over\" ; setLocalVarInt ui_highlight %d ; "
                 "setLocalVarString ui_choicegroup \"bo1_coop\" ; } leaveFocus { setLocalVarInt ui_highlight 0 ; } ", n);
    p.text += Fmt("action { play \"uin_navigation_click\" ; %s } } ", action.c_str());
    // its label in black over the highlight
    p.text += Fmt("itemDef { type 1 rect 15 %g %g 17 2 2 textfont 1 textscale 0.315 textalign 8 textalignx 17 "
                 "forecolor 0 0 0 1 text \"%s\" decoration visible when %s } ", y, w, label, highlighted.c_str());
}

static void Popup_End(CoopPopup &p)
{
    p.text += "} ";
}

// Host Game's choice of player count: Solo's mission select (main_online's Solo button), the count set first
static std::string Coop_HostAction(int players)
{
    return Fmt("close self ; setDvar bo1_coop_players %d ; setDvar menu_parent_3d_tv_name \"main_online\" ; "
              "setdvar next_menu_name \"levels_zombie\" ; exec \"set onlinegame 0; set systemlink 0\" ; "
              "open blackout_3d_tv ;", players);
}

static std::string UI_Coop_MenuText()
{
    CoopPopup online, host, join;

    Popup_Begin(online, "bo1_coop_online", 300.0f, 158.0f, "setfocus bo1_coop_online_host ;", "close self ;");
    Popup_Text(online, 15.0f, 20.0f, "ONLINE", nullptr);
    Popup_Text(online, 37.0f, 50.0f,
        "Play zombies together on a network or over the internet: one player hosts the game, the others join it.",
        nullptr);
    Popup_Button(online, "bo1_coop_online_host", 86.0f, "HOST GAME", "close self ; open bo1_coop_host ;");
    Popup_Button(online, "bo1_coop_online_join", 105.0f, "JOIN GAME", "close self ; open bo1_coop_join ;");
    Popup_Button(online, "bo1_coop_online_back", 124.0f, "BACK", "close self ;");
    Popup_End(online);

    Popup_Begin(host, "bo1_coop_host", 300.0f, 220.0f, "execNow \"bo1_coop_hostinfo\" ; setfocus bo1_coop_host_2 ;",
        "close self ; open bo1_coop_online ;");
    Popup_Text(host, 15.0f, 20.0f, "HOST GAME", nullptr);
    Popup_Text(host, 37.0f, 110.0f, nullptr, "bo1_coop_host_info");
    Popup_Button(host, "bo1_coop_host_2", 129.0f, "2 PLAYERS", Coop_HostAction(2));
    Popup_Button(host, "bo1_coop_host_3", 148.0f, "3 PLAYERS", Coop_HostAction(3));
    Popup_Button(host, "bo1_coop_host_4", 167.0f, "4 PLAYERS", Coop_HostAction(4));
    Popup_Button(host, "bo1_coop_host_back", 186.0f, "BACK", "close self ; open bo1_coop_online ;");
    Popup_End(host);

    Popup_Begin(join, "bo1_coop_join", 300.0f, 182.0f,
        "setDvar bo1_coop_join_info \"\" ; setfocus bo1_coop_join_address ;", "close self ; open bo1_coop_online ;");
    Popup_Text(join, 15.0f, 20.0f, "JOIN GAME", nullptr);
    Popup_Text(join, 37.0f, 30.0f,
        "The host's address: an IP address or a name, with :port after it if the host's port isn't 28960.", nullptr);
    // the address: typed at once (setfocus starts editing), Enter joins
    join.text += Fmt("itemDef { name \"bo1_coop_join_address\" type 5 rect 15 70 %g 20 2 2 style 1 backcolor 0 0 0 0.5 "
                    "border 1 bordersize 1 bordercolor 1 1 1 0.35 textfont 1 textscale 0.315 textalign 8 textalignx 6 "
                    "forecolor 1 1 1 1 text \"ADDRESS:\" dvar \"bo1_coop_join_address\" maxChars 64 visible 1 "
                    "accept { play \"uin_navigation_click\" ; exec \"bo1_coop_join\" ; } } ", join.w - 30.0f);
    Popup_Text(join, 96.0f, 30.0f, nullptr, "bo1_coop_join_info", "1 0.75 0.4 1"); // (what was wrong with it)
    Popup_Button(join, "bo1_coop_join_join", 129.0f, "JOIN", "exec \"bo1_coop_join\" ;");
    Popup_Button(join, "bo1_coop_join_back", 148.0f, "BACK", "close self ; open bo1_coop_online ;");
    Popup_End(join);

    return online.text + host.text + join.text;
}

// Host Game's text: what the count is for, and the address the others join
static void UI_Coop_HostInfo_f()
{
    char addresses[256];
    char port[16] = "";
    const int netPort = Dvar_GetInt("net_port");
    if ( netPort != BO1_DEFAULT_NET_PORT )
        Com_sprintf(port, sizeof(port), ":%d", netPort);
    std::string text = "How many players, counting you? The game starts when they have all joined. ";
    if ( NET_GetNetworkAddresses(addresses, sizeof(addresses), port) )
        text += va("On your network they join this computer's address: %s. ", addresses);
    else
        text += "On your network they join this computer's IP address (its network settings show it). ";
    text += va("Over the internet: your public IP address, with UDP port %d forwarded to this computer by your router.",
        netPort);
    Dvar_SetString((dvar_s *)bo1_coop_host_info, text.c_str());
}

// Join Game's text says what is wrong with the address, and the address is typed again (Enter in it ended its editing)
static void UI_Coop_JoinError(const char *message)
{
    Dvar_SetString((dvar_s *)bo1_coop_join_info, message);
    UiContext *dc = &UI_UIContext_GetInfo(0)->uiDC;
    menuDef_t *menu = Menus_FindByName(dc, "bo1_coop_join");
    if ( menu && menu->itemCount && Menus_MenuIsInStack(dc, menu) )
        Item_RunScript(0, dc, menu->items[0], "setfocus bo1_coop_join_address");
}

// bo1_coop_join [address]: joins the game at the address (default: the one typed in Join Game, bo1_coop_join_address)
static void UI_Coop_Join_f()
{
    char address[256];
    I_strncpyz(address, Cmd_Argc() > 1 ? Cmd_Argv(1) : (const char *)bo1_coop_join_address->current.string, sizeof(address));
    char *start = address;
    while ( *start == ' ' )
        ++start;
    for ( char *end = start + strlen(start); end > start && end[-1] == ' '; )
        *--end = 0;
    if ( !*start )
    {
        UI_Coop_JoinError("Type the host's address first.");
        return;
    }
    for ( const char *c = start; *c; ++c )
    {
        if ( !isalnum((unsigned char)*c) && !strchr(".:-_", *c) )
        {
            UI_Coop_JoinError("That isn't an address: an IP address is 4 numbers with dots between them (192.168.1.20).");
            return;
        }
    }
    Com_Printf(13, "co-op: joining %s\n", start);
    Cbuf_AddText(0, va("connect %s\n", start));
}

void UI_Coop_RegisterCommands()
{
    static cmd_function_s joinCmd;
    static cmd_function_s hostInfoCmd;
    static bool registered;
    if ( registered )
        return;
    registered = true;
    bo1_coop_join_address = _Dvar_RegisterString("bo1_coop_join_address", "", 1u,
        "co-op: the address typed in the zombies menu's Online > Join Game");
    bo1_coop_host_info = _Dvar_RegisterString("bo1_coop_host_info", "", 0, "co-op: the text of Online > Host Game");
    bo1_coop_join_info = _Dvar_RegisterString("bo1_coop_join_info", "", 0, "co-op: the text of Online > Join Game");
    Cmd_AddCommandInternal("bo1_coop_join", UI_Coop_Join_f, &joinCmd);
    Cmd_AddCommandInternal("bo1_coop_hostinfo", UI_Coop_HostInfo_f, &hostInfoCmd);
}

// the menus parsed into the hunk (with the front end's other menus, which a level's load clears) and their
// expressions into Z memory, freed at UI_Shutdown
static menuDef_t *s_coopMenus[8];
static int s_coopMenuCount;

// the Z memory of a popup's expressions (Menu_FreeMemory takes every item's type data for an image's)
static void UI_Coop_FreeMenu(menuDef_t *menu)
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

// main_online's Online buttons (online_button1, and its copy for some languages) open bo1_coop_online; its Solo
// buttons (online_button2) start a game that waits for nobody
static char s_onlineAction[] = "play \"uin_navigation_click\" ; open bo1_coop_online ;";
static char s_soloAction[1024];

static void UI_Coop_PatchMainOnline(UiContext *dc)
{
    menuDef_t *menu = Menus_FindByName(dc, "main_online");
    if ( !menu )
    {
        Com_PrintWarning(13, "co-op: no main_online menu, the zombies menu has no Online choice\n");
        return;
    }
    int patched = 0;
    for ( int i = 0; i < menu->itemCount; ++i )
    {
        itemDef_s *item = menu->items[i];
        if ( !item || !item->window.name )
            continue;
        const bool online = !I_stricmp(item->window.name, "online_button1");
        const bool solo = !I_stricmp(item->window.name, "online_button2");
        if ( !online && !solo )
            continue;
        for ( GenericEventHandler *handler = item->onEvent; handler; handler = handler->next )
        {
            if ( !handler->name || I_stricmp(handler->name, "action") || !handler->eventScript )
                continue;
            GenericEventScript *script = handler->eventScript;
            if ( online )
            {
                // (its scripts sign in to the online service and open its lobby: one script instead)
                script->action = s_onlineAction;
                script->type = 0;
                script->next = 0;
                ++patched;
            }
            else if ( script->action && script->action != s_soloAction )
            {
                if ( !s_soloAction[0] )
                    Com_sprintf(s_soloAction, sizeof(s_soloAction), "setDvar bo1_coop_players 0 ; %s",
                        (const char *)script->action);
                script->action = s_soloAction;
                ++patched;
            }
        }
    }
    Com_Printf(13, "co-op: zombies menu's Online and Solo buttons set up (%d)\n", patched);
}

void UI_Coop_LoadMenus(int localClientNum, UiContext *dc)
{
    UI_Coop_RegisterCommands();
    if ( Menus_FindByName(dc, "bo1_coop_online") )
    {
        UI_Coop_PatchMainOnline(dc); // (loaded already, the UI not shut down since)
        return;
    }
    s_coopMenuCount = 0; // (any earlier ones went with the UI's last shutdown)
    static std::string text;
    if ( text.empty() )
        text = UI_Coop_MenuText();
    MenuList *list = UI_LoadMenusFromMemory("bo1_coop.menu", text.c_str(), 3);
    for ( int i = 0; i < list->menuCount; ++i )
    {
        menuDef_t *menu = list->menus[i];
        if ( Menus_FindByName(dc, menu->window.name) || dc->menuCount >= 600 || s_coopMenuCount >= 8 )
        {
            UI_Coop_FreeMenu(menu);
            continue;
        }
        // (UI_AddMenu, without its test that a menu is a fast-file asset)
        dc->Menus[dc->menuCount++] = menu;
        s_coopMenus[s_coopMenuCount++] = menu;
        Menus_Close(localClientNum, dc, menu);
    }
    if ( s_coopMenuCount != 3 )
        Com_PrintWarning(13, "co-op: %d of the Online menus loaded\n", s_coopMenuCount);
    UI_Coop_PatchMainOnline(dc);
}

void UI_Coop_Shutdown(UiContext *dc)
{
    // (Com_ShutdownInternal closes the menus again after the hunk is cleared: Com_UnloadFrontEnd, UI_CloseAll)
    int kept = 0;
    for ( int i = 0; i < dc->menuCount; ++i )
    {
        bool coop = false;
        for ( int k = 0; k < s_coopMenuCount; ++k )
            coop = coop || dc->Menus[i] == s_coopMenus[k];
        if ( !coop )
            dc->Menus[kept++] = dc->Menus[i];
    }
    dc->menuCount = kept;
    for ( int i = 0; i < s_coopMenuCount; ++i )
    {
        if ( g_editItem && g_editItem->parent == s_coopMenus[i] )
        {
            g_editingField = 0;
            g_editItem = 0;
        }
        UI_Coop_FreeMenu(s_coopMenus[i]);
    }
    s_coopMenuCount = 0;
}
