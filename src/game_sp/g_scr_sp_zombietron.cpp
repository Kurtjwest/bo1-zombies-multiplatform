// SP builtin table, lane Z: what Dead Ops Arcade's scripts (zombietron.ff: maps/_zombietron_*.gsc) call that no other
// table has. Searched after BO1Zombies's own tables and before the generated not-ported table (see
// src/game_sp/scr_sp_tables.h). Written from what the scripts expect of each one (the SP exe's own versions are not
// in the decompile): the arguments and the effect DOA relies on.
#include "scr_sp_tables.h"
#include "g_scr_sp_zombietron.h"
#include <clientscript/cscr_vm.h>
#include <clientscript/scr_const.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_mp/g_spawn_mp.h>
#include <game/actor_corpse.h>
#include <game/g_weapon.h>
#include <universal/com_math_anglevectors.h>
#include <universal/dvar.h>
#include <universal/q_shared.h>

// game_mp/g_scr_main_mp.cpp: magicbullet's zombiemode firing (SP G_f_magicbullet)
gentity_s *G_SP_MagicBulletFire(int weapon, const float *source, const float *dest, gentity_s *attacker,
    gentity_s *targetEnt, const float *targetOffset);

// the score multiplier of each player (setscoremultiplier: DOA packs (bar progress << 4) | (multiplier - 1)); the
// HUD gets it through a scoreboard column (G_PopulateMatchState)
static int s_scoreMultiplier[64];
// the weapon each entity fires with fire() (setweapon), 0: none
static int s_entWeapon[1024];

int G_SP_ScoreMultiplier(int clientNum)
{
    return clientNum >= 0 && clientNum < (int)ARRAY_COUNT(s_scoreMultiplier) ? s_scoreMultiplier[clientNum] : 0;
}

void G_SP_ZombietronReset()
{
    memset(s_scoreMultiplier, 0, sizeof(s_scoreMultiplier));
    memset(s_entWeapon, 0, sizeof(s_entWeapon));
}

static gentity_s *ZT_PlayerEntity(scr_entref_t entref)
{
    if ( entref.classnum )
    {
        Scr_ObjectError("not an entity", SCRIPTINSTANCE_SERVER);
        return nullptr;
    }
    gentity_s *ent = &g_entities[entref.entnum];
    if ( !ent->client )
    {
        Scr_ObjectError(va("entity %i is not a player", entref.entnum), SCRIPTINSTANCE_SERVER);
        return nullptr;
    }
    return ent;
}

// GetDvarVector( <dvar> ): a vector dvar's value (the top-down camera's player_topDownCamAngles, ...), as the client's
// GetDvarVector (cg_scr_main.cpp)
static void G_f_getdvarvector()
{
    const dvar_s *dvar;
    if ( Scr_GetType(0, SCRIPTINSTANCE_SERVER) == 6 )
        dvar = Dvar_FindVar(Scr_GetInt(0, SCRIPTINSTANCE_SERVER));
    else
        dvar = Dvar_FindVar(Scr_GetString(0, SCRIPTINSTANCE_SERVER));
    if ( !dvar )
    {
        Scr_Error(SCRIPTINSTANCE_SERVER, "Invalid Dvar passed into GetDvarVector", false);
        return;
    }
    if ( dvar->type != DVAR_TYPE_FLOAT_3 )
    {
        float v[3] = { 0.0f, 0.0f, 0.0f };
        if ( dvar->type == DVAR_TYPE_FLOAT_4 || dvar->type == DVAR_TYPE_FLOAT_2 )
        {
            v[0] = dvar->current.vector[0];
            v[1] = dvar->current.vector[1];
            v[2] = dvar->type == DVAR_TYPE_FLOAT_4 ? dvar->current.vector[2] : 0.0f;
        }
        else
        {
            sscanf(Dvar_DisplayableValue(dvar), "%f %f %f", &v[0], &v[1], &v[2]);
        }
        Scr_AddVector(v, SCRIPTINSTANCE_SERVER);
        return;
    }
    Scr_AddVector((float *)&dvar->current.value, SCRIPTINSTANCE_SERVER);
}

// ClearAllCorpses(): every actor corpse removed (DOA between arenas)
static void G_f_clearallcorpses()
{
    G_RemoveAllActorCorpses();
}

// SetWeapon( <entity>, <weapon> ): the weapon the entity fires with fire() (DOA's double-shot orb copies the player's)
static void G_f_setweapon()
{
    gentity_s *ent = Scr_GetEntity(0);
    const char *name = Scr_GetString(1, SCRIPTINSTANCE_SERVER);
    const int weapon = G_GetWeaponIndexForName((char *)name);
    if ( !weapon )
    {
        Scr_Error(SCRIPTINSTANCE_SERVER, va("SetWeapon: unknown weapon %s", name), false);
        return;
    }
    if ( ent && ent->s.number < ARRAY_COUNT(s_entWeapon) )
        s_entWeapon[ent->s.number] = weapon;
}

// Fire( <entity>, [owner] ): one shot of its weapon (SetWeapon) from its tag_flash (or origin), the way it faces,
// owned by owner (the player whose orb it is)
static void G_f_fire()
{
    gentity_s *ent = Scr_GetEntity(0);
    if ( !ent || ent->s.number >= ARRAY_COUNT(s_entWeapon) || !s_entWeapon[ent->s.number] )
        return;
    gentity_s *owner = Scr_GetNumParam(SCRIPTINSTANCE_SERVER) > 1 && Scr_GetType(1, SCRIPTINSTANCE_SERVER)
                           ? Scr_GetEntity(1) : &g_entities[ENTITYNUM_WORLD];
    float source[3];
    float forward[3];
    float dest[3];
    float offset[3] = { 0.0f, 0.0f, 0.0f };
    if ( !G_DObjGetWorldTagPos(ent, scr_const.tag_flash, source) )
    {
        source[0] = ent->r.currentOrigin[0];
        source[1] = ent->r.currentOrigin[1];
        source[2] = ent->r.currentOrigin[2];
    }
    AngleVectors(ent->r.currentAngles, forward, nullptr, nullptr);
    dest[0] = source[0] + forward[0] * 8192.0f;
    dest[1] = source[1] + forward[1] * 8192.0f;
    dest[2] = source[2] + forward[2] * 8192.0f;
    G_SP_MagicBulletFire(s_entWeapon[ent->s.number], source, dest, owner, nullptr, offset);
}

// Objective_String( <id>, <text> ) / Objective_Set3D( <id>, <on>, [color], [text] ): SP's 3D objective markers (DOA's
// tutorial text, the arenas' exits). Not drawn: the arguments are taken so the scripts run.
static void G_f_objective_string()
{
}

static void G_f_objective_set3d()
{
}

// <player> ClearDamageIndicator(): the damage this frame shows no indicator (DOA ignores some hits: shield, splash)
static void G_m_cleardamageindicator(scr_entref_t entref)
{
    gentity_s *ent = ZT_PlayerEntity(entref);
    if ( !ent )
        return;
    ent->client->damage_blood = 0;
    ent->client->damage_fromWorld = 0;
    ent->client->ps.damageCount = 0;
}

// <player> GetScoreMultiplier() / SetScoreMultiplier( <value> )
static void G_m_getscoremultiplier(scr_entref_t entref)
{
    gentity_s *ent = ZT_PlayerEntity(entref);
    Scr_AddInt(ent ? G_SP_ScoreMultiplier(ent->s.number) : 0, SCRIPTINSTANCE_SERVER);
}

static void G_m_setscoremultiplier(scr_entref_t entref)
{
    gentity_s *ent = ZT_PlayerEntity(entref);
    if ( ent && ent->s.number < (int)ARRAY_COUNT(s_scoreMultiplier) )
        s_scoreMultiplier[ent->s.number] = Scr_GetInt(0, SCRIPTINSTANCE_SERVER);
}

const BuiltinFunctionDef g_sp_zombietron_functions[] =
{
    { "getdvarvector", G_f_getdvarvector, 0 },
    { "clearallcorpses", G_f_clearallcorpses, 0 },
    { "setweapon", G_f_setweapon, 0 },
    { "fire", G_f_fire, 0 },
    { "objective_string", G_f_objective_string, 0 },
    { "objective_set3d", G_f_objective_set3d, 0 },
    { nullptr, nullptr, 0 } // end marker, keep last
};
const unsigned int g_sp_zombietron_function_count = ARRAY_COUNT(g_sp_zombietron_functions) - 1;

const BuiltinMethodDef g_sp_zombietron_methods[] =
{
    { "cleardamageindicator", G_m_cleardamageindicator, 0 },
    { "getscoremultiplier", G_m_getscoremultiplier, 0 },
    { "setscoremultiplier", G_m_setscoremultiplier, 0 },
    { nullptr, nullptr, 0 } // end marker, keep last
};
const unsigned int g_sp_zombietron_method_count = ARRAY_COUNT(g_sp_zombietron_methods) - 1;
