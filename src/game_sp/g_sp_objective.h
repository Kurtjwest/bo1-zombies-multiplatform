#pragma once
// zombies: SP's objectives on SP levels (g_sp_objective.cpp)

void G_SP_InitObjectives();
void G_SP_SendObjectiveChanges();          // end of the server frame: the changed info strings to every client
void G_SP_SendObjectivesToClient(int clientNum); // a client that begins: all of them

// KB's MP objective builtins (g_scr_main_mp.cpp) call these on SP levels
void G_SP_Objective_Add();
void G_SP_Objective_Delete();
void G_SP_Objective_State();
void G_SP_Objective_Icon();
void G_SP_Objective_Position();
void G_SP_Objective_OnEntity();
void G_SP_Objective_Current();
