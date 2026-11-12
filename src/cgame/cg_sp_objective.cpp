// zombies: SP's 3D objectives on the client. The server's objective info strings (SP config strings 0x142 + n,
// game_sp/g_sp_objective.cpp) arrive in 'O' <n> "<info>" commands and are parsed as SP's CG_ParseObjectiveChange
// (SP 0x005d4280) parses them; SP's HUD menus draw them with ownerdraw 322 (CG_Draw3DObjectives, SP 0x00643260):
// on screen a pointer and the text (or the distance; "*" draws nothing), off screen an arrow at the edge toward it.
// Dead Ops Arcade marks its exits ("*") and the tutorial's goal this way.
#include "cg_sp_objective.h"
#include <cgame_mp/cg_local_mp.h>
#include <cgame_mp/cg_main_mp.h>
#include <cgame/cg_hudelem.h>
#include <client/screen_placement.h>
#include <client_mp/cl_cgame_mp.h>
#include <client_mp/cl_main_mp.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_ui3d.h>
#include <qcommon/cmd.h>
#include <qcommon/common.h>
#include <stringed/stringed_hooks.h>
#include <ui/ui_main.h>
#include <universal/com_math.h>
#include <universal/dvar.h>
#include <universal/q_shared.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct cg_objective3d_t // SP cg + 0xa9d98 + 0x8a4 * n
{
    int state;              // 0 empty, 1 active, 2 invisible, 3 done, 4 current, 5 failed
    float origin[8][3];     // "org%d"
    int entNum[8];          // "ent%d", -1 = none
    char str[1024];         // "str"
    int changeTime;         // cg time when it became current or its ring changed, -1 cleared
    int ring;
    int icon;
    int use3d;              // bit 0 on, bits 1..4 the ui3d window + 1 (0 = the screen)
    int col3d;              // 0 cg_objectiveIndicatorColor, 1 white, 2 red, 3 green, 4 blue, 5 ltblue
    char txt3d[1024];       // "" = the distance, "*" = nothing on screen
    float offset[3];
};

static cg_objective3d_t s_objectives3d[MAX_LOCAL_CLIENTS][32];

// SP registers these at 0x004a5e93 .. 0x004a602b and 0x004a6589
static const dvar_t *hud_showObjectives;
static const dvar_t *cg_objectiveIndicatorColor;
static const dvar_t *cg_objectiveIndicatorNearFadeDist;
static const dvar_t *cg_objectiveIndicatorFarFadeDist;
static const dvar_t *cg_objectiveIndicatorNearDist;
static const dvar_t *cg_objectiveIndicatorFarDist;
static const dvar_t *cg_objectiveIndicatorFarScale;
static const dvar_t *cg_objectiveIndicatorSize;

void CG_SP_RegisterObjectiveDvars()
{
    hud_showObjectives = _Dvar_RegisterBool("hud_showObjectives", 1, 0x1000u, "");
    cg_objectiveIndicatorColor = _Dvar_RegisterVec4("cg_objectiveIndicatorColor", 1.0f, 0.94f, 0.0f, 1.0f, 0.0f, 1.0f,
        0x80u, "");
    cg_objectiveIndicatorNearFadeDist = _Dvar_RegisterFloat("cg_objectiveIndicatorNearFadeDist", 256.0f, 0.0f,
        3.4028235e38f, 0x1000u, "");
    cg_objectiveIndicatorFarFadeDist = _Dvar_RegisterFloat("cg_objectiveIndicatorFarFadeDist", 8192.0f, 0.0f,
        3.4028235e38f, 0x1000u, "");
    cg_objectiveIndicatorNearDist = _Dvar_RegisterFloat("cg_objectiveIndicatorNearDist", 256.0f, 0.0f, 3.4028235e38f,
        0x1000u, "");
    cg_objectiveIndicatorFarDist = _Dvar_RegisterFloat("cg_objectiveIndicatorFarDist", 512.0f, 0.0f, 3.4028235e38f,
        0x1000u, "");
    cg_objectiveIndicatorFarScale = _Dvar_RegisterFloat("cg_objectiveIndicatorFarScale", 0.5f, 0.0f, 3.4028235e38f,
        0x1000u, "");
    cg_objectiveIndicatorSize = _Dvar_RegisterFloat("cg_objectiveIndicatorSize", 0.8f, 0.0f, 100.0f, 0x1000u, "");
}

static void CG_SP_ClearObjective3D(cg_objective3d_t *obj)
{
    // (col3d and offset stay, as in SP)
    memset(obj->origin, 0, sizeof(obj->origin));
    for ( int i = 0; i < 8; ++i )
        obj->entNum[i] = -1;
    obj->str[0] = 0;
    obj->changeTime = -1;
    obj->ring = 0;
    obj->icon = 0;
    obj->use3d = 0;
    obj->txt3d[0] = 0;
}

void CG_SP_InitObjectives(int localClientNum)
{
    for ( int n = 0; n < 32; ++n )
    {
        cg_objective3d_t *obj = &s_objectives3d[localClientNum][n];
        memset(obj, 0, sizeof(*obj));
        CG_SP_ClearObjective3D(obj);
    }
}

static int CG_SP_InfoInt(char *info, const char *key)
{
    const char *value = Info_ValueForKey(info, key);
    return *value ? atoi(value) : 0;
}

// SP 0x005d4280
static void CG_SP_ParseObjectiveChange(int localClientNum, int objNum, char *info)
{
    const cg_s *cgameGlob = CG_GetLocalClientGlobals(localClientNum);
    cg_objective3d_t *obj = &s_objectives3d[localClientNum][objNum];
    if ( !*info )
    {
        obj->state = 0;
        CG_SP_ClearObjective3D(obj);
        return;
    }
    const char *team = Info_ValueForKey(info, "t");
    if ( *team && atoi(team) != cgameGlob->bgs.clientinfo[cgameGlob->clientNum].team )
        return;
    const int oldState = obj->state;
    obj->state = CG_SP_InfoInt(info, "state");
    if ( !obj->state )
    {
        CG_SP_ClearObjective3D(obj);
        return;
    }
    I_strncpyz(obj->str, Info_ValueForKey(info, "str"), sizeof(obj->str));
    for ( int i = 0; i < 8; ++i )
    {
        char key[16];
        Com_sprintf(key, sizeof(key), "org%d", i);
        const char *org = Info_ValueForKey(info, key);
        if ( *org )
            sscanf(org, "%f %f %f", &obj->origin[i][0], &obj->origin[i][1], &obj->origin[i][2]);
        else
            obj->origin[i][0] = obj->origin[i][1] = obj->origin[i][2] = 0.0f;
        Com_sprintf(key, sizeof(key), "ent%d", i);
        const char *ent = Info_ValueForKey(info, key);
        obj->entNum[i] = *ent ? atoi(ent) : -1;
    }
    const int ring = CG_SP_InfoInt(info, "ring");
    if ( (obj->state == 4 && oldState != 4) || ring != obj->ring )
        obj->changeTime = cgameGlob->time;
    obj->ring = ring;
    obj->icon = CG_SP_InfoInt(info, "icon");
    obj->use3d = CG_SP_InfoInt(info, "use3d");
    obj->col3d = CG_SP_InfoInt(info, "col3d");
    obj->offset[0] = (float)CG_SP_InfoInt(info, "offx");
    obj->offset[1] = (float)CG_SP_InfoInt(info, "offy");
    obj->offset[2] = (float)CG_SP_InfoInt(info, "offz");
    I_strncpyz(obj->txt3d, Info_ValueForKey(info, "txt3d"), sizeof(obj->txt3d));
}

// 'O' <n> "<info>"
void CG_SP_ObjectiveCommand(int localClientNum)
{
    const int objNum = atoi(Cmd_Argv(1));
    if ( objNum < 0 || objNum >= 32 )
        return;
    char info[1024];
    I_strncpyz(info, Cmd_Argv(2), sizeof(info));
    CG_SP_ParseObjectiveChange(localClientNum, objNum, info);
}

// SP 0x00643260, ownerdraw 322
void CG_SP_Draw3DObjectives(int localClientNum)
{
    if ( zombietron && zombietron->current.enabled && localClientNum )
        return;
    if ( !hud_showObjectives || !hud_showObjectives->current.enabled )
        return;
    cg_s *cgameGlob = CG_GetLocalClientGlobals(localClientNum);
    const ScreenPlacement *scrPlace = &scrPlaceView[localClientNum];
    Font_s *font = UI_GetFontHandle(scrPlace, 1, 1.0f);
    const float avg = (scrPlace->scaleVirtualToReal[0] + scrPlace->scaleVirtualToReal[1]) * 0.5f;
    const float pointerW = waypointOffscreenPointerWidth->current.value * scrPlace->scaleVirtualToReal[0];
    const float pointerH = waypointOffscreenPointerHeight->current.value * scrPlace->scaleVirtualToReal[1];
    const float pointerDist = waypointOffscreenPointerDistance->current.value * avg;
    float alphaThresh = waypointOffscreenDistanceThresholdAlpha->current.value * avg;
    if ( alphaThresh < 0.1f )
        alphaThresh = 0.1f;
    const float pad = pointerH * 0.5f + pointerDist;
    const float padL = waypointOffscreenPadLeft->current.value * scrPlace->scaleVirtualToReal[0] + pad;
    const float padR = waypointOffscreenPadRight->current.value * scrPlace->scaleVirtualToReal[0] + pad;
    const float padT = waypointOffscreenPadTop->current.value * scrPlace->scaleVirtualToReal[1] + pad;
    const float padB = waypointOffscreenPadBottom->current.value * scrPlace->scaleVirtualToReal[1] + pad;
    Material *offscreenPointer = Material_RegisterHandle("hud_offscreenobjectivepointer", 7);
    Material *onscreenPointer = Material_RegisterHandle("hud_onscreenobjectivepointer", 7);

    for ( int n = 0; n < 32; ++n )
    {
        const cg_objective3d_t *obj = &s_objectives3d[localClientNum][n];
        if ( !(obj->use3d & 1) )
            continue;
        const int window = ((obj->use3d >> 1) & 0xf) - 1;
        bool extraCam = false;
        if ( window >= 0 )
        {
            if ( window != R_UI3DStack_Top(R_GetUI3DStack()) )
                continue;
            extraCam = true;
        }
        if ( obj->state != 1 && obj->state != 4 )
            continue;

        for ( int i = 0; i < 8; ++i )
        {
            float pos[3];
            if ( obj->entNum[i] != -1 )
            {
                const centity_s *cent = CG_GetEntity(localClientNum, obj->entNum[i]);
                pos[0] = cent->pose.origin[0] + obj->offset[0];
                pos[1] = cent->pose.origin[1] + obj->offset[1];
                pos[2] = cent->pose.origin[2] + obj->offset[2];
                if ( cent->nextState.eType == ET_PLAYER || cent->nextState.eType == ET_ACTOR )
                {
                    if ( (cent->currentState.eFlags & 8) != 0 )
                        pos[2] += waypointPlayerOffsetProne->current.value;
                    else if ( (cent->currentState.eFlags & 4) != 0 )
                        pos[2] += waypointPlayerOffsetCrouch->current.value;
                    else
                        pos[2] += waypointPlayerOffsetStand->current.value;
                }
            }
            else
            {
                if ( obj->origin[i][0] == 0.0f && obj->origin[i][1] == 0.0f && obj->origin[i][2] == 0.0f )
                    continue;
                pos[0] = obj->origin[i][0] + obj->offset[0];
                pos[1] = obj->origin[i][1] + obj->offset[1];
                pos[2] = obj->origin[i][2] + obj->offset[2];
            }
            if ( pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f )
                continue;

            float color[4];
            switch ( obj->col3d )
            {
            case 1: color[0] = 1.0f; color[1] = 1.0f; color[2] = 1.0f; color[3] = 1.0f; break;
            case 2: color[0] = 1.0f; color[1] = 0.0f; color[2] = 0.0f; color[3] = 1.0f; break;
            case 3: color[0] = 0.0f; color[1] = 1.0f; color[2] = 0.0f; color[3] = 1.0f; break;
            case 4: color[0] = 0.0f; color[1] = 0.0f; color[2] = 1.0f; color[3] = 1.0f; break;
            case 5: color[0] = 0.87843f; color[1] = 0.95f; color[2] = 0.9f; color[3] = 1.0f; break;
            default:
                color[0] = cg_objectiveIndicatorColor->current.vector[0];
                color[1] = cg_objectiveIndicatorColor->current.vector[1];
                color[2] = cg_objectiveIndicatorColor->current.vector[2];
                color[3] = cg_objectiveIndicatorColor->current.vector[3];
                break;
            }
            float iconColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

            float screenPos[2], clampedDir[2], clampedDist;
            if ( !extraCam )
                WorldPosToScreenPos(localClientNum, pos, screenPos);
            else
                WorldPosToExtraCamScreenPos(localClientNum, pos, screenPos);
            if ( ClampScreenPosToEdges(localClientNum, screenPos, padL, padR, padT, padB, clampedDir, &clampedDist)
                && clampedDist > 0.0f )
            {
                // off the screen: the arrow at the edge (for "*" too)
                const float dist = Vec3Distance(pos, cgameGlob->refdef.vieworg);
                float arrowColor[4] = { color[0], color[1], color[2], color[3] };
                if ( alphaThresh > clampedDist )
                    arrowColor[3] = color[3] / alphaThresh * clampedDist;
                const float ax = clampedDir[0] * pointerDist + screenPos[0];
                const float ay = clampedDir[1] * pointerDist + screenPos[1];
                const float angle = (float)atan2(clampedDir[0], -clampedDir[1]) * 57.2958f;
                CL_DrawStretchPicPhysicalRotateXYW(ax - pointerW * 0.5f, ay - pointerH * 0.5f, dist, pointerW, pointerH,
                    0.0f, 0.0f, 1.0f, 1.0f, angle, arrowColor, offscreenPointer);
                continue;
            }

            // on the screen
            const float dist = Vec3Distance(pos, cgameGlob->refdef.vieworg);
            char distText[20];
            char *text;
            if ( obj->txt3d[0] )
            {
                text = SEH_LocalizeTextMessage(obj->txt3d, "objective text", LOCMSG_SAFE);
                if ( text[0] == '*' )
                    continue;
            }
            else
            {
                const float meters = dist * 0.0254f;
                Com_sprintf(distText, sizeof(distText), meters < 10.0f ? "%0.1fm" : "%0.0fm", meters);
                text = distText;
            }
            // the fade, as SP has it (not clamped; the far fade divides by the near fade distance)
            if ( cg_objectiveIndicatorNearFadeDist->current.value > dist )
                color[3] = iconColor[3] = 1.0f - (cg_objectiveIndicatorNearDist->current.value - dist)
                                               / cg_objectiveIndicatorNearDist->current.value;
            else if ( dist > cg_objectiveIndicatorFarFadeDist->current.value )
                color[3] = iconColor[3] = 1.0f - (dist - cg_objectiveIndicatorFarFadeDist->current.value)
                                               / cg_objectiveIndicatorNearFadeDist->current.value;
            float s = 1.0f;
            if ( dist >= cg_objectiveIndicatorNearDist->current.value )
            {
                float t = (dist - cg_objectiveIndicatorNearDist->current.value)
                        / (cg_objectiveIndicatorFarDist->current.value - cg_objectiveIndicatorNearDist->current.value);
                t = t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t);
                s = cg_objectiveIndicatorFarScale->current.value * t + (1.0f - t);
            }
            // (SP scales by 2/3 x 1.5 when a 16-bit global, 0x03e5e69a, is 1080; nothing in the PC exe writes it)
            const float yScale = (float)R_NormalizedTextScale(font, cg_objectiveIndicatorSize->current.value * s);
            const float hiResScale = (float)ScrPlace_HiResGetScale();
            const float xScale = hiResScale * (yScale / hiResScale * scrPlace->scaleRealToVirtual[1])
                               * scrPlace->scaleVirtualToReal[0];
            CL_DrawStretchPicPhysicalRotateXYW(screenPos[0] - 7.5f, screenPos[1], dist, 15.0f, 15.0f, 0.0f, 0.0f, 1.0f,
                1.0f, 0.0f, iconColor, onscreenPointer);
            const float w = (float)R_TextWidth(text, 0x7fffffff, font) * xScale * 0.5f;
            CL_DrawTextPhysical(text, 0x7fffffff, font, screenPos[0] - w, screenPos[1], dist, xScale, yScale, color, 3);
        }
    }
}
