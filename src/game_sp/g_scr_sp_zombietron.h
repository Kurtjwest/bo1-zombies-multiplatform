#pragma once
// zombies: Dead Ops Arcade (zombietron): its builtins' state (g_scr_sp_zombietron.cpp)

// a player's score multiplier as DOA's scripts packed it (setscoremultiplier): (bar progress << 4) | (multiplier - 1)
int G_SP_ScoreMultiplier(int clientNum);
// a new level: no multipliers
void G_SP_ZombietronReset();
