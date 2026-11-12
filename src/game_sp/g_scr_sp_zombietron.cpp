// SP builtin table, lane Z: what Dead Ops Arcade's scripts (zombietron.ff: maps/_zombietron_*.gsc) call that no other
// table has. Searched after BO1Zombies's own tables and before the generated not-ported table (see
// src/game_sp/scr_sp_tables.h). Ported from the SP exe (addresses below).
#include "scr_sp_tables.h"
#include "g_scr_sp_zombietron.h"
#include <clientscript/cscr_vm.h>
#include <clientscript/scr_const.h>
#include <game_mp/g_main_mp.h>
#include <game_mp/g_utils_mp.h>
#include <game_mp/g_spawn_mp.h>
#include <game/actor_corpse.h>
#include <game/bullet.h>
#include <game/g_weapon.h>
#include <bgame/bg_misc.h>
#include <bgame/bg_weapons_load_obj.h>
#include <universal/com_math.h>
#include <universal/com_math_anglevectors.h>
#include <universal/dvar.h>
#include <universal/q_shared.h>

// each player's score multiplier (setscoremultiplier: DOA packs (bar progress << 4) | (multiplier - 1)). SP keeps it in
// the clientState score (scoreMultiplier, +0xc8, 14 bits on the wire); BO1Zombies's MP score has no such field, so it
// lives here and reaches the HUD in a scoreboard column (G_PopulateMatchState)
static int s_scoreMultiplier[64];

int G_SP_ScoreMultiplier(int clientNum)
{
    return clientNum >= 0 && clientNum < (int)ARRAY_COUNT(s_scoreMultiplier) ? s_scoreMultiplier[clientNum] : 0;
}

void G_SP_ZombietronReset()
{
    memset(s_scoreMultiplier, 0, sizeof(s_scoreMultiplier));
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

// GetDvarVector( <dvar> ) (SP 0x007f0380): a vector dvar's value (the top-down camera's player_topDownCamAngles, ...),
// as the client's (cg_scr_main.cpp)
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
        Scr_Error(SCRIPTINSTANCE_SERVER, "dvar is not a vector in but GetDvarVec has been called on it", false);
    Scr_AddVector((float *)&dvar->current.value, SCRIPTINSTANCE_SERVER);
}

// ClearAllCorpses() (SP 0x008003d0: jmp G_RemoveAllActorCorpses): every actor corpse freed (DOA between arenas)
static void G_f_clearallcorpses()
{
    G_RemoveAllActorCorpses();
}

// SetWeapon( <entity>, <weapon> ) (SP 0x0052cd10): the entity's own (networked) weapon, the one fire() shoots and the
// client's EV_FIRE_WEAPON flash and sound use (DOA's double-shot orb copies the player's)
static void G_f_setweapon()
{
    gentity_s *ent = Scr_GetEntity(0);
    const char *name = Scr_GetString(1, SCRIPTINSTANCE_SERVER);
    const int weapon = G_GetWeaponIndexForName((char *)name);
    if ( !weapon )
        Scr_Error(SCRIPTINSTANCE_SERVER, va("SetWeapon called with unknown weapon name %s\n", name), false);
    ent->s.weapon = (unsigned char)weapon;
}

// Fire( <entity>, <attacker> ) (SP 0x00472990): one shot of the entity's weapon from its origin, the way it faces,
// owned by the attacker. Bullets ignore the entity (it is their weapon entity), a flamethrower fires from it;
// shotgun-class weapons spread by their fHipSpreadStandMin. The entity gets the EV_FIRE_WEAPON event.
static void G_f_fire()
{
    gentity_s *ent = Scr_GetEntity(0);
    gentity_s *attacker = Scr_GetEntity(1);
    const int weapon = ent->s.weapon;
    weaponParms wp;
    float zeroOffset[3] = { 0.0f, 0.0f, 0.0f };
    float spread = 0.0f;

    Weapon_SetWeaponParamsWeapon(&wp, weapon);
    AngleVectors(ent->r.currentAngles, wp.forward, nullptr, nullptr);
    wp.muzzleTrace[0] = ent->r.currentOrigin[0];
    wp.muzzleTrace[1] = ent->r.currentOrigin[1];
    wp.muzzleTrace[2] = ent->r.currentOrigin[2];
    memset(wp.right, 0, 24); // right and up
    if ( wp.weapDef->weapType == WEAPTYPE_GRENADE || wp.weapDef->weapType == WEAPTYPE_MINE )
        Scr_Error(SCRIPTINSTANCE_SERVER, "Fire() does not work with grenade-type weapons.\n", false);
    if ( wp.weapDef->weapClass == WEAPCLASS_SPREAD )
    {
        spread = wp.weapDef->fHipSpreadStandMin;
        PerpendicularVector(wp.forward, wp.right);
        Vec3Cross(wp.forward, wp.right, wp.up);
    }
    switch ( wp.weapDef->weapType )
    {
    case WEAPTYPE_BULLET:
        Bullet_Fire(attacker, spread, &wp, ent, level.time);
        break;
    case WEAPTYPE_PROJECTILE:
        if ( wp.weapDef->weapClass == WEAPCLASS_GRENADE )
            Weapon_GrenadeLauncher_Fire(attacker, weapon, 0, &wp);
        else if ( wp.weapDef->weapClass == WEAPCLASS_PISTOL || wp.weapDef->weapClass == WEAPCLASS_ROCKETLAUNCHER
                  || wp.weapDef->weapClass == WEAPCLASS_TURRET )
            Weapon_RocketLauncher_Fire(attacker, weapon, 0.0f, &wp, vec3_origin, nullptr, zeroOffset);
        else
            Scr_Error(SCRIPTINSTANCE_SERVER, "Fire(): Unhandled projectile weapClass.\n", false);
        break;
    case WEAPTYPE_GAS:
        Weapon_Flamethrower_Fire(ent, &wp);
        break;
    default:
        Scr_Error(SCRIPTINSTANCE_SERVER, va("Fire(): Unhandled weapType \"%s\".\n", BG_GetWeaponTypeName(wp.weapDef->weapType)),
            false);
        break;
    }
    G_AddEvent(ent, EV_FIRE_WEAPON, 0);
}

// Objective_String( <id>, <text> ) (SP 0x007f70b0) / Objective_Set3D( <id>, <on>, [color], [text], ... ) (SP
// 0x007f7530): SP's objective config strings (key "str"; "use3d", colour, "txt3d", offsets), which its client draws as
// an on-screen marker with the text, or an arrow toward an objective off the screen (SP 0x00643260). Not ported:
// BO1Zombies's config strings and objective fields have no room for them. In Dead Ops Arcade the text is "*", which SP
// doesn't draw, and the arenas' exits are on the screen, so SP shows nothing for them there either.
static void G_f_objective_string()
{
}

static void G_f_objective_set3d()
{
}

// <player> ClearDamageIndicator() (SP 0x007d9a90): this frame's damage gives no indicator or flinch (P_DamageFeedback
// reads damage_blood at the end of the frame; DOA ignores some hits)
static void G_m_cleardamageindicator(scr_entref_t entref)
{
    gentity_s *ent = ZT_PlayerEntity(entref);
    if ( !ent )
        return;
    ent->client->damage_blood = 0;
}

// <player> GetScoreMultiplier() / SetScoreMultiplier( <value> ) (SP 0x007d9bc0 / 0x007d9be0): no checks, the entity
// number is the client number
static void G_m_getscoremultiplier(scr_entref_t entref)
{
    Scr_AddInt(G_SP_ScoreMultiplier(entref.entnum), SCRIPTINSTANCE_SERVER);
}

static void G_m_setscoremultiplier(scr_entref_t entref)
{
    const int value = Scr_GetInt(0, SCRIPTINSTANCE_SERVER);
    if ( entref.entnum < ARRAY_COUNT(s_scoreMultiplier) )
        s_scoreMultiplier[entref.entnum] = value;
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
