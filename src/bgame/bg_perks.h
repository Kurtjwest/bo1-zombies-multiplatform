#pragma once

#include <universal/dvar.h>
#include <universal/q_shared.h>

unsigned int __cdecl BG_GetPerkIndexForName(const char *perkName);
const char *__cdecl BG_GetPerkNameForIndex(unsigned int perkIndex);
void __cdecl Perks_RegisterDvars();

// zombies: endurance has no MP perk. Reserve unused bit 52 in the existing 64-bit
// PS/session perk wire storage; BG_HasPerk's 52 named MP perks remain unchanged.
const unsigned int BG_SP_ENDURANCE_PERK_MASK = 1u << 20; // perks[1]
// mod: LinkerMod's game_mod perks that have no MP perk either, in the bits after it (perks[1], sent with the player
// state): specialty_fastoffhand (offhand throws at perk_fastOffhandRate) and specialty_stockpile (two more magazines of
// reserve ammo for each weapon). Its other four are MP perks under other names (G_SP_SetPerk)
const unsigned int BG_SP_FASTOFFHAND_PERK_MASK = 1u << 21; // perks[1]
const unsigned int BG_SP_STOCKPILE_PERK_MASK = 1u << 22;   // perks[1]
extern const dvar_t *perk_weapRateEnhanced; // mod: game_mod's Double Tap 2.0 (two bullets a shot)
extern const dvar_t *perk_fastOffhandRate;  // mod: game_mod's specialty_fastoffhand rate

extern const dvar_t *perk_skillSet;
extern const dvar_t *perk_weapSpreadMultiplier;
extern const dvar_t *perk_weapRateMultiplier;
extern const dvar_t *perk_weapReloadMultiplier;
extern const dvar_t *perk_weapSwitchMultiplier;
extern const dvar_t *perk_weapAdsMultiplier;
extern const dvar_t *perk_weapMeleeMultiplier;
extern const dvar_t *perk_extraBreath;
extern const dvar_t *perk_bulletPenetrationMultiplier;
extern const dvar_t *perk_grenadeDeath;
extern const dvar_t *perk_sprintMultiplier;
extern const dvar_t *perk_grenadeTossBackTimer;
extern const dvar_t *perk_killstreakReduction;
extern const dvar_t *perk_extraMoneyMultiplier;
extern const dvar_t *perk_delayExplosiveTime;
extern const dvar_t *perk_disarmExplosiveTime;
extern const dvar_t *perk_speedMultiplier;
extern const dvar_t *perk_deathStreakCountRequired;
extern const dvar_t *perk_sprintRecoveryMultiplier;
extern const dvar_t *perk_healthRegenMultiplier;
extern const dvar_t *perk_damageKickReduction;
extern const dvar_t *perk_shellShockReduction;
extern const dvar_t *perk_mantleReduction;
extern const dvar_t *perk_interactSpeedMultiplier;
extern const dvar_t *perk_throwbackInnerRadius;
extern const dvar_t *perk_flakJacket;
extern const dvar_t *perk_flakJacket_hardcore;
extern const dvar_t *perk_fireproof;
extern const dvar_t *perk_armorPiercing;
extern const dvar_t *perk_dogsAttackGhost;
extern const dvar_t *perk_blackbirdShowsGpsJammer;

inline bool __cdecl BG_HasPerk(const unsigned int *perks, unsigned int perkIndex)
{
    if (perkIndex >= 0x34
        && !Assert_MyHandler(
            "c:\\projects_pc\\cod\\codsrc\\src\\bgame\\../bgame/bg_perks.h",
            136,
            0,
            "perkIndex doesn't index PERK_COUNT\n\t%i not in [0, %i)",
            perkIndex,
            52))
    {
        __debugbreak();
    }
    return (*(_QWORD *)perks & (1LL << perkIndex)) != 0;
}
