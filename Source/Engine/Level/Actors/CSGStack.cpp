// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGStack.h"

CSGStack::CSGStack(const SpawnParams& params)
    : CSGScopeActor(params)
{
    _name = TEXT("CSG Stack");
}
