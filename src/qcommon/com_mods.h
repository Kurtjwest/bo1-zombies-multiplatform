#pragma once

// mod: the Mods menu (main menu > Mods, src/ui/ui_mods.cpp). Every folder mods/<name> with a mod.txt is a mod the menu
// lists (README.md "Mods menu" has the format). Turned on, a mod is stacked with fs_mods when a game starts from the
// menus (Solo, Host Game, Join Game): Mods_ApplyForMap, from SV_SpawnServer and, for a joining player, from
// CL_ParseGamestate. The menu's choices are archived dvars (bo1_mods_<name>, each option's own dvar).

#include <string>
#include <vector>

struct dvar_s;

// a setting a mod (or one of its option values) makes: "set" when a game starts (only on map, if named), "startup" when
// the game itself starts (memory and pools sized then: the menu asks for a restart)
struct ModSetting
{
    std::string dvar;
    std::string value;
    std::string map;
    bool startup;
};

// a zone file the mod (or an option value) needs installed, on one map or every map
struct ModNeed
{
    std::string zone;
    std::string label;
    std::string map;
};

struct ModValue
{
    std::string value;
    std::string label;
    std::vector<ModSetting> settings;
    std::vector<ModNeed> needs;
};

struct ModOption
{
    std::string dvar;
    std::string label;
    std::string help;
    std::string type; // toggle, choice, range
    std::string def;
    std::vector<ModValue> values;
    const dvar_s *var;
};

struct ModMap
{
    std::string name;
    std::string label;
};

struct ModInfo
{
    std::string folder; // its name: fs_mods', the menu's and the dvars' (the folder's name, other characters than
                        // letters, digits and '_' as '_': FS_ModIdForFolder)
    std::string dir;    // its folder, mods/<dir> ("Rust MW2" for Rust_MW2)
    std::string name;
    std::string description;
    int order;
    std::vector<ModMap> maps; // none: every map
    std::vector<ModNeed> needs;
    std::vector<ModSetting> settings;
    std::vector<ModOption> options;
    std::vector<std::string> clears; // dvars its scripts make (name, or prefix*): empty again when each game starts
    const dvar_s *enabled; // bo1_mods_<folder>
    // a map mod (a mod-tools map: mods/<dir>/<map>.ff, maybe a mod.ff and .iwd files; mod.txt optional): played from
    // its page (Play, Host Game), not turned on or off; stacked with fs_mods in every game on its map(s)
    bool mapMod;
    std::string version; // mod.json's "vers"
};

// WinMain, before PMem_Init: reads the startup settings the Mods menu saved (players/mods_startup.cfg) unless the command
// line picks the mods itself (fs_mods / fs_game) or is a test run (+map, +devmap, headless, dedicated)
void Com_ModStartup_Init(const char *cmdline);
// extra zones the memory and the asset pools are sized for: bo1_mod_zones on the command line, or the Mods menu's
int Com_ModStartup_ZonesReserved();
// " +set <dvar> <value>" for each startup setting the Mods menu saved ("" if none)
const char *Com_ModStartup_SetArgs();
// fs_mods or fs_game on the command line: the launcher picked the mods, the Mods menu's choices are not used
bool Com_ModStartup_CommandLineMods();

// the mods in mods/ with a mod.txt (read again by Mods_Discover), in fs_mods order
const std::vector<ModInfo> &Mods_List();
void Mods_Discover();
// the front end's menus loaded: from now on the Mods menu's choices apply to the games started
void Mods_SetMenuActive();
bool Mods_MenuActive();
// before a level loads (server, or a client joining one): fs_mods and the enabled mods' settings for that map
void Mods_ApplyForMap(const char *mapName);

// what the menu shows: a mod's state, why, and what a restart would change
bool Mods_IsEnabled(const ModInfo &mod);
bool Mods_IsAvailable(const ModInfo &mod, std::string *why); // its zone files are installed (on at least one map)
int Mods_OptionValueIndex(const ModOption &option); // the option's current value in its list (-1: not one of them)
std::string Mods_RestartReason(const ModInfo &mod); // "" if this session has what the mod's current choices need
std::string Mods_MapList(const ModInfo &mod);
// the map mod that has map's zone (nullptr: none)
const ModInfo *Mods_MapModFor(const char *map);
// writes players/mods_startup.cfg if what the enabled mods need at startup changed; true if a restart is needed
bool Mods_SaveStartup(std::string *who);
void Mods_RegisterCommands();
