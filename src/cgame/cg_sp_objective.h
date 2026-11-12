#pragma once
// zombies: SP's 3D objectives on the client (cg_sp_objective.cpp)

void CG_SP_RegisterObjectiveDvars();
void CG_SP_InitObjectives(int localClientNum);
void CG_SP_ObjectiveCommand(int localClientNum); // 'O' <n> "<info>" (game_sp/g_sp_objective.cpp)
void CG_SP_Draw3DObjectives(int localClientNum); // SP ownerdraw 322
