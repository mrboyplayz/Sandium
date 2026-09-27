#pragma once

#include <subhook.h>
#include "../structs/Common.hpp"

extern subhook::Hook *createEventBulletHitHook;
void CreateEventBulletHitHookFunc(int source, int hitType,
                                  structs::CVector3 *position,
                                  structs::CVector3 *normal);

#if _VSCODE
#define IMPLEMENT_HOOKS 1
#endif

#if IMPLEMENT_HOOKS && _WIN32

#include "../Addresses.hpp"
#include "../api/BloodMarks.hpp"

subhook::Hook *createEventBulletHitHook;

void CreateEventBulletHitHookFunc(int source, int hitType,
                                  structs::CVector3 *position,
                                  structs::CVector3 *normal)
{
    subhook::ScopedHookRemove scopedRemove(createEventBulletHitHook);
    addresses::CreateEventBulletHitFunc(source, hitType, position, normal);

    if (hitType != 3 || !position || !*addresses::IsInGame ||
        !addresses::LineIntersectLevelFunc.ptr ||
        !addresses::LineIntersectResult.ptr)
        return;

    structs::CVector3 start(position->x, position->y + 0.3f, position->z);
    structs::CVector3 end(position->x, position->y - 3.0f, position->z);
    const structs::LineIntersectResult saved = *addresses::LineIntersectResult.ptr;
    const bool hit = addresses::LineIntersectLevelFunc(&start, &end, 1) != 0;
    const structs::LineIntersectResult result = *addresses::LineIntersectResult.ptr;
    *addresses::LineIntersectResult.ptr = saved;
    if (hit)
        api::bloodmarks::Add(result.position.x, result.position.y,
                             result.position.z, result.normal.y);
}

#endif
