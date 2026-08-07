// Copyright (c) Wojciech Figat. All rights reserved.

#include "GroupActor.h"

GroupActor::GroupActor(const SpawnParams& params)
    : EmptyActor(params)
{
}

#if USE_EDITOR

BoundingBox GroupActor::GetEditorBox() const
{
    return BoundingBox::Empty;
}

#endif
