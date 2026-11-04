#pragma once
struct UiContext;
struct menuDef_t;

// mod: the zombies menu's Mission Select (levels_zombie) with the map mods turned on in the Mods menu, after the game's
// maps, and a list that scrolls (src/ui/ui_levels.cpp). With the SP front end's menus, after the Mods menu's (the mods
// read): adds the rows to the game's menu
void UI_Levels_LoadMenus(int localClientNum, UiContext *dc);
// levels_zombie back as it was, our items' memory freed (UI_Shutdown: the hunk they are in is cleared next)
void UI_Levels_Shutdown(UiContext *dc);
// Menu_Paint, before a menu is drawn (after its lists are placed, UI_Scroll_Frame): for levels_zombie, the panel of the
// map mod whose row has the focus, or the game's
void UI_Levels_Frame(int localClientNum, UiContext *dc, menuDef_t *menu);
// the rows' "uiScript bo1LevelsPlay <mod>" (UI_RunMenuScript); false if name is not it
bool UI_Levels_MenuScript(int localClientNum, UiContext *dc, const char *name, const char **args);
