// zombies: SP's objectives (SP 0x007f65e0 .. 0x007f7e52, the objective_* builtins). On SP levels they replace KB's MP
// objective builtins (Scr_Objective_* in g_scr_main_mp.cpp hand over to these) and add SP's others to the SP builtin
// tables. SP keeps each objective twice: level.objectives[n] (MP's objective_t layout: state, origin, entNum, team,
// icon, use3D, color) and an info string in config string 0x142 + n ("state", "str", "org%d" / "ent%d", "icon",
// "ring", "t", and objective_set3d's "txt3d", "use3d", "col3d", "offx/y/z"), which its client parses
// (CG_ParseObjectiveChange, SP 0x005d4280) and draws (ownerdraw 322, SP 0x00643260).
//
// BO1Zombies's MP config strings have no free range of 32 (0x142 is MP's CS_CODINFO_VALUE_LAST), so the info strings
// live here and go to the clients in an 'O' <n> "<info>" server command, at the end of the server frame for each one
// that changed (SP's config strings likewise reach a client once per snapshot), and all of them to a client that
// begins (CG_SP_ObjectiveCommand, cgame/cg_sp_objective.cpp).
#include "g_sp_objective.h"
#include "scr_sp_tables.h"
#include "g_sp_levelstart.h"
#include <game_mp/g_main_mp.h>
#include <game_mp/g_scr_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_mp/g_spawn_mp.h>
#include <server/sv_game.h>
#include <server_mp/sv_main_mp.h>
#include <clientscript/cscr_vm.h>
#include <clientscript/cscr_stringlist.h>
#include <clientscript/scr_const.h>
#include <qcommon/common.h>
#include <universal/q_shared.h>
#include <cctype>
#include <cstring>

static char s_objectiveInfo[32][1024]; // SP config strings 0x142 + n
static bool s_objectiveDirty[32];

static void G_SP_GetObjectiveInfo(int objNum, char *info)
{
    I_strncpyz(info, s_objectiveInfo[objNum], 1024);
}

// SV_SetConfigstring: the change reaches the clients at the end of the frame (G_SP_SendObjectiveChanges)
static void G_SP_SetObjectiveInfo(int objNum, const char *info)
{
    if ( !strcmp(s_objectiveInfo[objNum], info) )
        return;
    I_strncpyz(s_objectiveInfo[objNum], info, 1024);
    s_objectiveDirty[objNum] = true;
}

static void G_SP_SendObjectiveInfo(int clientNum, int objNum)
{
    SV_GameSendServerCommand(clientNum, SV_CMD_RELIABLE, va("%c %i \"%s\"", 'O', objNum, s_objectiveInfo[objNum]));
}

void G_SP_InitObjectives()
{
    memset(s_objectiveInfo, 0, sizeof(s_objectiveInfo));
    memset(s_objectiveDirty, 0, sizeof(s_objectiveDirty));
}

void G_SP_SendObjectiveChanges()
{
    for ( int objNum = 0; objNum < 32; ++objNum )
    {
        if ( !s_objectiveDirty[objNum] )
            continue;
        s_objectiveDirty[objNum] = false;
        G_SP_SendObjectiveInfo(-1, objNum);
    }
}

void G_SP_SendObjectivesToClient(int clientNum)
{
    for ( int objNum = 0; objNum < 32; ++objNum )
    {
        if ( s_objectiveInfo[objNum][0] )
            G_SP_SendObjectiveInfo(clientNum, objNum);
    }
}

// SP 0x00546000: the objective message (a 'V' command), sent only outside zombiemode, where no BO1Zombies level runs
// objective scripts (the SP front end has none)
static void G_SP_ObjectiveMessage(unsigned int /*state*/, const char * /*text*/, int /*clientNum*/)
{
}

static int G_SP_ObjectiveIndex(unsigned int paramIndex)
{
    const int objNum = Scr_GetInt(paramIndex, SCRIPTINSTANCE_SERVER);
    if ( objNum < 0 || objNum >= 32 )
        Scr_Error(va("index %i is an illegal objective index. Valid indexes are 0 to %i\n", objNum, 31), 0);
    return objNum;
}

// (objective_icon, objective_onentity, objective_team: Scr_ParamError instead of Scr_Error)
static int G_SP_ObjectiveIndexParam(unsigned int paramIndex)
{
    const int objNum = Scr_GetInt(paramIndex, SCRIPTINSTANCE_SERVER);
    if ( objNum < 0 || objNum >= 32 )
        Scr_ParamError(paramIndex, va("index %i is an illegal objective index. Valid indexes are 0 to %i\n", objNum, 31),
            SCRIPTINSTANCE_SERVER);
    return objNum;
}

// the entity an objective was on stops being broadcast for it (svFlags 0x10)
static void G_SP_ClearObjectiveEntity(objective_t *obj)
{
    if ( obj->entNum == 1023 )
        return;
    gentity_s *ent = &g_entities[obj->entNum];
    if ( ent->r.inuse )
        ent->r.svFlags &= ~0x10u;
    obj->entNum = 1023;
}

// SP 0x007f6660 / 0x007f65e0: the icon parameter, checked, as the material index
static int G_SP_ObjectiveIconIndex(unsigned int paramIndex)
{
    const char *name = Scr_GetString(paramIndex, SCRIPTINSTANCE_SERVER);
    int i = 0;
    for ( ; name[i]; ++i )
    {
        if ( name[i] <= 0x1f || name[i] >= 0x7f )
            Scr_ParamError(3, va("Illegal character '%c'(ascii %i) in objective icon name: %s\n", name[i],
                (unsigned char)name[i], name), SCRIPTINSTANCE_SERVER);
    }
    if ( i >= 64 )
        Scr_ParamError(3, va("Objective icon name is too long (> %i): %s\n", 63, name), SCRIPTINSTANCE_SERVER);
    return G_MaterialIndex(name);
}

// the "str" / "txt3d" text: a localized string reference, or a plain string (with loc_warnings off, used as it is)
static const char *G_SP_ObjectiveText(unsigned int paramIndex)
{
    const char *text;
    if ( Scr_GetType(paramIndex, SCRIPTINSTANCE_SERVER) == 3 )
    {
        text = Scr_GetIString(paramIndex, SCRIPTINSTANCE_SERVER);
        const int len = (int)strlen(text);
        if ( len > 1 )
        {
            for ( int i = 0; i < len; ++i )
            {
                if ( !isalnum((unsigned char)text[i]) && text[i] != '_' )
                    Scr_ParamError(paramIndex, va("Illegal localized string reference: %s (must contain only alpha-numeric "
                        "characters and underscores", text), SCRIPTINSTANCE_SERVER);
            }
        }
    }
    else
    {
        text = Scr_GetString(paramIndex, SCRIPTINSTANCE_SERVER);
        for ( int i = 0; text[i]; ++i )
        {
            if ( text[i] == 20 || text[i] == 21 || text[i] == 22 )
                Scr_ParamError(paramIndex, va("bad escape character (%i) present in string", text[i]), SCRIPTINSTANCE_SERVER);
        }
    }
    return text;
}

// SP's check of an objective text (inlined in objective_add, objective_string and objective_set3d); the last two
// build it with Scr_ConstructMessageString and let its 0x14/0x15 token separators through
static void G_SP_ValidateObjectiveText(const char *text, bool messageString)
{
    const int len = (int)strlen(text);
    if ( len >= 1024 )
        Scr_Error(va("Objective strings is too long (> %i): %s\n", 1023, text), 0);
    if ( strchr(text, '"') || strchr(text, ';') || strchr(text, '\\') )
        Scr_Error(va("Objective strings can not have a \", a ;, or a \\ in them. Illegal objective string: %s\n", text), 0);
    for ( int i = 0; i < len; ++i )
    {
        const unsigned char c = (unsigned char)text[i];
        if ( messageString && (c == 0x14 || c == 0x15) )
            continue;
        if ( (c >= 1 && c <= 0x1f) || c >= 0x7f )
            Scr_Error(va("Illegal character '%c'(ascii %i) in objective string: %s\n", text[i], c, text), 0);
    }
}

// the position parameter: a point ("org<n>", without "ent<n>") or an entity ("ent<n>", without "org<n>")
static void G_SP_ObjectivePositionParam(char *info, unsigned int paramIndex, int slot, objective_t *obj)
{
    char key[16];
    if ( Scr_GetType(paramIndex, SCRIPTINSTANCE_SERVER) == 4 )
    {
        float origin[3];
        Scr_GetVector(paramIndex, origin, SCRIPTINSTANCE_SERVER);
        Com_sprintf(key, sizeof(key), "org%d", slot);
        Info_SetValueForKey(info, key, va("%i %i %i", (int)origin[0], (int)origin[1], (int)origin[2]));
        Com_sprintf(key, sizeof(key), "ent%d", slot);
        Info_RemoveKey(info, key);
        if ( obj )
        {
            obj->origin[0] = origin[0];
            obj->origin[1] = origin[1];
            obj->origin[2] = origin[2];
        }
    }
    else
    {
        gentity_s *ent = Scr_GetEntity(paramIndex);
        Com_sprintf(key, sizeof(key), "ent%d", slot);
        Info_SetValueForKey(info, key, va("%i", ent->s.number));
        Com_sprintf(key, sizeof(key), "org%d", slot);
        Info_RemoveKey(info, key);
        if ( obj )
            obj->entNum = ent->s.number;
    }
}

// SP 0x007f6700: objective_add( index, state, [string], [position], [icon] ), objective_add_team (team first)
static void G_SP_Objective_AddInternal(int paramOffset, int team)
{
    char info[1024];
    const int numParam = Scr_GetNumParam(SCRIPTINSTANCE_SERVER);
    if ( numParam < paramOffset + 2 )
        Scr_Error("objective_add needs at least the first two parameters out of its parameter list of: index state "
                  "[string] [position]\n", 0);
    const int objNum = G_SP_ObjectiveIndex(paramOffset);
    G_SP_GetObjectiveInfo(objNum, info);
    const unsigned int stateName = Scr_GetConstString(paramOffset + 1, SCRIPTINSTANCE_SERVER);
    objectiveState_t state;
    if ( !ObjectiveStateIndexFromString(&state, stateName) )
        Scr_Error(va("Illegal objective state \"%s\". Valid states are \"empty\", \"active\", \"invisible\", \"done\", "
                     "\"current\", \"failed\"\n", Scr_GetString(paramOffset + 1, SCRIPTINSTANCE_SERVER)), 0);
    Info_SetValueForKey(info, "state", va("%i", state));
    objective_t *obj = &level.objectives[objNum];
    G_SP_ClearObjectiveEntity(obj);
    obj->state = state;
    obj->alt_3D_text = nullptr;
    obj->use3D = 0;
    const char *message = "GAME_OBJECTIVESUPDATED";
    if ( numParam > paramOffset + 2 )
    {
        const char *text = G_SP_ObjectiveText(paramOffset + 2);
        G_SP_ValidateObjectiveText(text, false);
        Info_SetValueForKey(info, "str", text);
        message = text;
        if ( numParam > paramOffset + 3 )
        {
            G_SP_ObjectivePositionParam(info, paramOffset + 3, 0, obj);
            if ( numParam > paramOffset + 4 )
            {
                const int icon = G_SP_ObjectiveIconIndex(paramOffset + 4);
                Info_SetValueForKey(info, "icon", va("%i", icon));
                obj->icon = icon;
            }
        }
    }
    if ( team == -1 )
    {
        G_SP_SetObjectiveInfo(objNum, info);
        SV_GameSendServerCommand(-1, SV_CMD_CAN_IGNORE, va("%c \"%i\"", 'J', 3)); // the compass menu fades in
        G_SP_ObjectiveMessage(stateName, message, -1);
        obj->teamNum = 0;
        return;
    }
    Info_SetValueForKey(info, "t", va("%i", team));
    obj->teamNum = team;
    G_SP_SetObjectiveInfo(objNum, info);
    for ( int clientNum = 0; clientNum < level.maxclients; ++clientNum )
    {
        const gentity_s *ent = &g_entities[clientNum];
        if ( svs.clients[clientNum].header.state < CS_CONNECTED || !ent->client || ent->client->sess.cs.team != team )
            continue;
        SV_GameSendServerCommand(clientNum, SV_CMD_CAN_IGNORE, va("%c \"%i\"", 'J', 3));
        G_SP_ObjectiveMessage(stateName, message, clientNum);
    }
}

void G_SP_Objective_Add()
{
    G_SP_Objective_AddInternal(0, -1);
}

// SP 0x007f6c20: objective_add_team( team, index, state, ... ); a team other than allies / axis is "none" (-1)
static void G_f_objective_add_team()
{
    const unsigned int team = Scr_GetConstString(0, SCRIPTINSTANCE_SERVER);
    if ( team == scr_const.allies )
        G_SP_Objective_AddInternal(1, 2);
    else if ( team == scr_const.axis )
        G_SP_Objective_AddInternal(1, 1);
    else
        G_SP_Objective_AddInternal(1, -1);
}

// SP 0x007f6c70: the info string's "state" becomes empty (its other keys stay), the objective is cleared
void G_SP_Objective_Delete()
{
    char info[4];
    const int objNum = G_SP_ObjectiveIndex(0);
    // SP reads 4 bytes of the config string and writes it back emptied
    info[0] = 0;
    G_SP_SetObjectiveInfo(objNum, info);
    objective_t *obj = &level.objectives[objNum];
    G_SP_ClearObjectiveEntity(obj);
    obj->state = OBJST_EMPTY;
    obj->origin[0] = 0.0f;
    obj->origin[1] = 0.0f;
    obj->origin[2] = 0.0f;
    obj->size[0] = 0.0f;
    obj->size[1] = 0.0f;
    obj->entNum = 1023;
    obj->teamNum = 0;
    obj->icon = 0;
}

// SP 0x007f6d50
void G_SP_Objective_State()
{
    char info[1024];
    const int objNum = G_SP_ObjectiveIndex(0);
    G_SP_GetObjectiveInfo(objNum, info);
    const unsigned int stateName = Scr_GetConstString(1, SCRIPTINSTANCE_SERVER);
    objectiveState_t state;
    if ( !ObjectiveStateIndexFromString(&state, stateName) )
        Scr_Error(va("Illegal objective state \"%s\". Valid states are \"empty\", \"active\", \"invisible\", \"done\", "
                     "\"current\", \"failed\"\n", Scr_GetString(1, SCRIPTINSTANCE_SERVER)), 0);
    Info_SetValueForKey(info, "state", va("%i", state));
    G_SP_SetObjectiveInfo(objNum, info);
    objective_t *obj = &level.objectives[objNum];
    obj->state = state;
    if ( state == OBJST_EMPTY || state == OBJST_INVISIBLE )
        G_SP_ClearObjectiveEntity(obj);
    G_SP_ObjectiveMessage(stateName, "GAME_OBJECTIVESUPDATED", -1);
}

// SP 0x007f6e80: objective_string( index, text... ) / objective_string_nomessage
static void G_SP_Objective_StringInternal(bool withMessage)
{
    char info[1024];
    char text[1024];
    const int objNum = G_SP_ObjectiveIndex(0);
    Scr_ConstructMessageString(1, Scr_GetNumParam(SCRIPTINSTANCE_SERVER) - 1, "Objective String", text, 1024);
    G_SP_ValidateObjectiveText(text, true);
    G_SP_GetObjectiveInfo(objNum, info);
    Info_SetValueForKey(info, "str", text);
    G_SP_SetObjectiveInfo(objNum, info);
    if ( withMessage )
        G_SP_ObjectiveMessage(scr_const.active, text, -1);
}

static void G_f_objective_string()
{
    G_SP_Objective_StringInternal(true);
}

static void G_f_objective_string_nomessage()
{
    G_SP_Objective_StringInternal(false);
}

// SP 0x007f70d0
void G_SP_Objective_Icon()
{
    char info[1024];
    const int objNum = G_SP_ObjectiveIndexParam(0);
    G_SP_GetObjectiveInfo(objNum, info);
    const int icon = G_SP_ObjectiveIconIndex(1);
    Info_SetValueForKey(info, "icon", va("%i", icon));
    G_SP_SetObjectiveInfo(objNum, info);
    level.objectives[objNum].icon = icon;
}

// SP 0x007f7160: the objective's first position; level.objectives keeps the point (snapped to whole units), no entity
void G_SP_Objective_Position()
{
    char info[1024];
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    const int objNum = G_SP_ObjectiveIndex(0);
    G_SP_GetObjectiveInfo(objNum, info);
    const bool isPoint = Scr_GetType(1, SCRIPTINSTANCE_SERVER) == 4;
    if ( isPoint )
        Scr_GetVector(1, origin, SCRIPTINSTANCE_SERVER);
    G_SP_ObjectivePositionParam(info, 1, 0, nullptr);
    G_SP_SetObjectiveInfo(objNum, info);
    objective_t *obj = &level.objectives[objNum];
    G_SP_ClearObjectiveEntity(obj);
    if ( isPoint )
    {
        obj->origin[0] = (float)(int)origin[0];
        obj->origin[1] = (float)(int)origin[1];
        obj->origin[2] = (float)(int)origin[2];
    }
}

// SP 0x007f7320: objective_additionalposition( index, position index 0..7, position )
static void G_f_objective_additionalposition()
{
    char info[1024];
    const int objNum = G_SP_ObjectiveIndex(0);
    const int slot = Scr_GetInt(1, SCRIPTINSTANCE_SERVER);
    if ( slot < 0 || slot >= 8 )
        Scr_Error(va("index %i is an illegal position index. Valid indexes are 0 to %i\n", slot, 7), 0);
    G_SP_GetObjectiveInfo(objNum, info);
    G_SP_ObjectivePositionParam(info, 2, slot, nullptr);
    G_SP_SetObjectiveInfo(objNum, info);
}

// SP 0x007f74a0: level.objectives only (the info string's position is not changed)
void G_SP_Objective_OnEntity()
{
    const int objNum = G_SP_ObjectiveIndexParam(0);
    objective_t *obj = &level.objectives[objNum];
    G_SP_ClearObjectiveEntity(obj);
    gentity_s *ent = Scr_GetEntity(1);
    ent->r.svFlags |= 0x10u;
    obj->entNum = ent->s.number;
}

// SP 0x007f7990 + 0x007f78e0: the listed objectives become current, the others that were current active
void G_SP_Objective_Current()
{
    int makeCurrent[32];
    char info[1024];
    const int numParam = Scr_GetNumParam(SCRIPTINSTANCE_SERVER);
    memset(makeCurrent, 0, sizeof(makeCurrent));
    for ( int i = 0; i < numParam; ++i )
        makeCurrent[G_SP_ObjectiveIndex(i)] = 1;
    for ( int objNum = 0; objNum < 32; ++objNum )
    {
        G_SP_GetObjectiveInfo(objNum, info);
        const char *stateStr = Info_ValueForKey(info, "state");
        const int state = *stateStr ? atoi(stateStr) : 0;
        if ( makeCurrent[objNum] )
        {
            if ( state == OBJST_CURRENT )
                continue;
            Info_SetValueForKey(info, "state", va("%i", OBJST_CURRENT));
        }
        else
        {
            if ( state != OBJST_CURRENT )
                continue;
            Info_SetValueForKey(info, "state", va("%i", OBJST_ACTIVE));
        }
        G_SP_SetObjectiveInfo(objNum, info);
    }
    // SP 0x007f78e0 reads the parameters again (a bad index is a Scr_ParamError there)
    for ( int objNum = 0; objNum < 32; ++objNum )
    {
        objective_t *obj = &level.objectives[objNum];
        if ( makeCurrent[objNum] )
            obj->state = OBJST_CURRENT;
        else if ( obj->state == OBJST_CURRENT )
            obj->state = OBJST_ACTIVE;
    }
    G_SP_ObjectiveMessage(scr_const.current, "GAME_OBJECTIVESUPDATED", -1);
}

// SP 0x007f7ba0: the listed objectives become current, the others keep their state
static void G_f_objective_additionalcurrent()
{
    int makeCurrent[32];
    char info[1024];
    const int numParam = Scr_GetNumParam(SCRIPTINSTANCE_SERVER);
    memset(makeCurrent, 0, sizeof(makeCurrent));
    for ( int i = 0; i < numParam; ++i )
        makeCurrent[G_SP_ObjectiveIndex(i)] = 1;
    for ( int objNum = 0; objNum < 32; ++objNum )
    {
        if ( !makeCurrent[objNum] )
            continue;
        G_SP_GetObjectiveInfo(objNum, info);
        const char *stateStr = Info_ValueForKey(info, "state");
        if ( (*stateStr ? atoi(stateStr) : 0) == OBJST_CURRENT )
            continue;
        Info_SetValueForKey(info, "state", va("%i", OBJST_CURRENT));
        G_SP_SetObjectiveInfo(objNum, info);
    }
    G_SP_ObjectiveMessage(scr_const.current, "GAME_OBJECTIVESUPDATED", -1);
}

// SP 0x007f7cc0: "ring" toggles (the compass ring)
static void G_f_objective_ring()
{
    char info[1024];
    const int objNum = G_SP_ObjectiveIndex(0);
    G_SP_GetObjectiveInfo(objNum, info);
    const char *ring = Info_ValueForKey(info, "ring");
    Info_SetValueForKey(info, "ring", va("%i", (*ring ? atoi(ring) : 0) == 0));
    G_SP_SetObjectiveInfo(objNum, info);
}

// SP 0x007f7d70: objective_size( index, entity number ) (its bounds' width and depth) / ( index, width, depth )
static void G_f_objective_size()
{
    if ( Scr_GetNumParam(SCRIPTINSTANCE_SERVER) < 2 )
        Scr_Error("Objective_Size() called with wrong params.\n", 0);
    const int objNum = G_SP_ObjectiveIndex(0);
    objective_t *obj = &level.objectives[objNum];
    if ( Scr_GetNumParam(SCRIPTINSTANCE_SERVER) == 2 )
    {
        const unsigned int entNum = (unsigned int)Scr_GetInt(1, SCRIPTINSTANCE_SERVER);
        if ( entNum >= 1024 || !g_entities[entNum].r.inuse )
            return;
        const gentity_s *ent = &g_entities[entNum];
        obj->size[0] = ent->r.absmax[0] - ent->r.absmin[0];
        obj->size[1] = ent->r.absmax[1] - ent->r.absmin[1];
        return;
    }
    const float width = Scr_GetFloat(1, SCRIPTINSTANCE_SERVER);
    const float depth = Scr_GetFloat(2, SCRIPTINSTANCE_SERVER);
    obj->size[0] = width;
    obj->size[1] = depth;
}

// SP 0x007f7530: objective_set3d( index, on, [color], [text], [ui3d window], [offset] )
static void G_f_objective_set3d()
{
    char info[1024];
    char text[1024] = "";
    float offset[3] = { 0.0f, 0.0f, 0.0f };
    const int numParam = Scr_GetNumParam(SCRIPTINSTANCE_SERVER);
    const int objNum = G_SP_ObjectiveIndex(0);
    objective_t *obj = &level.objectives[objNum];
    obj->use3D = 0;
    obj->color = 0;
    G_SP_GetObjectiveInfo(objNum, info);
    switch ( numParam )
    {
    case 6:
        Scr_GetVector(5, offset, SCRIPTINSTANCE_SERVER);
        [[fallthrough]];
    case 5:
    {
        const int window = Scr_GetInt(4, SCRIPTINSTANCE_SERVER);
        obj->use3D |= (((window < 0) ? 0 : window + 1) & 0xf) << 1;
        [[fallthrough]];
    }
    case 4:
        Scr_ConstructMessageString(3, 3, "Objective String", text, 1024);
        G_SP_ValidateObjectiveText(text, true);
        [[fallthrough]];
    case 3:
    {
        const char *color = Scr_GetString(2, SCRIPTINSTANCE_SERVER);
        if ( !I_stricmp(color, "white") )
            obj->color = 1;
        else if ( !I_stricmp(color, "red") )
            obj->color = 2;
        else if ( !I_stricmp(color, "green") )
            obj->color = 3;
        else if ( !I_stricmp(color, "blue") )
            obj->color = 4;
        else if ( !I_stricmp(color, "ltblue") )
            obj->color = 5;
        [[fallthrough]];
    }
    case 2:
        obj->use3D |= Scr_GetInt(1, SCRIPTINSTANCE_SERVER) & 1;
        break;
    default:
        Scr_Error(va("Invalid argument count. Expected 2-6, given %d\n", numParam), 0);
        break;
    }
    Info_SetValueForKey(info, "txt3d", text);
    Info_SetValueForKey(info, "use3d", va("%i", obj->use3D));
    Info_SetValueForKey(info, "col3d", va("%i", obj->color));
    Info_SetValueForKey(info, "offx", va("%i", (int)offset[0]));
    Info_SetValueForKey(info, "offy", va("%i", (int)offset[1]));
    Info_SetValueForKey(info, "offz", va("%i", (int)offset[2]));
    G_SP_SetObjectiveInfo(objNum, info);
}

const BuiltinFunctionDef g_sp_objective_functions[] =
{
    { "objective_add_team", G_f_objective_add_team, 0 },
    { "objective_string", G_f_objective_string, 0 },
    { "objective_string_nomessage", G_f_objective_string_nomessage, 0 },
    { "objective_additionalposition", G_f_objective_additionalposition, 0 },
    { "objective_additionalcurrent", G_f_objective_additionalcurrent, 0 },
    { "objective_ring", G_f_objective_ring, 0 },
    { "objective_size", G_f_objective_size, 0 },
    { "objective_set3d", G_f_objective_set3d, 0 },
    { nullptr, nullptr, 0 } // end marker, keep last
};
const unsigned int g_sp_objective_function_count = ARRAY_COUNT(g_sp_objective_functions) - 1;

const BuiltinMethodDef g_sp_objective_methods[] =
{
    { nullptr, nullptr, 0 } // end marker, keep last
};
const unsigned int g_sp_objective_method_count = ARRAY_COUNT(g_sp_objective_methods) - 1;
