#pragma once
struct UiContext;

// co-op: the zombies menu's Online choice (host a game, or join one on a network or over the internet)
void UI_Coop_RegisterCommands();
// with the SP front end's menus: adds the Online popups and points main_online's buttons at them
void UI_Coop_LoadMenus(int localClientNum, UiContext *dc);
// takes the popups out of the context's menus and frees their memory (UI_Shutdown: the hunk they are in is cleared
// next)
void UI_Coop_Shutdown(UiContext *dc);
