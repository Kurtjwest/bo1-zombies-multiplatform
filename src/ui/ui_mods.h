#pragma once
struct UiContext;

// mod: the main menu's Mods screen (src/qcommon/com_mods.h has the mods). With the SP front end's menus: reads mods/
// again, adds the Mods popup and one page per mod (text menus, as the co-op popups)
void UI_Mods_LoadMenus(int localClientNum, UiContext *dc);
// takes the popups out of the context's menus and frees their memory (UI_Shutdown: the hunk they are in is cleared next)
void UI_Mods_Shutdown(UiContext *dc);
// the menus' "uiScript bo1Mods..." commands (UI_RunMenuScript); false if name is not one of them
bool UI_Mods_MenuScript(int localClientNum, UiContext *dc, const char *name, const char **args);
// main_text's Mods row: opens the Mods popup
extern char g_modsMenuOpenScript[];
