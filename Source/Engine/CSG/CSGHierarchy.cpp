// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGHierarchy.h"
#include "Engine/Level/Actor.h"
#include "Engine/Level/Actors/CSGScopeActor.h"
#include "Engine/Level/Actors/CSGStack.h"
#include "Engine/CSG/Brush.h"

using namespace CSG;

Actor* CSGHierarchy::FindOwningStackScope(const Actor* actor)
{
    if (actor == nullptr)
        return nullptr;

    const Actor* current = actor->GetParent();
    while (current != nullptr)
    {
        auto scope = dynamic_cast<const CSGScopeActor*>(current);
        if (scope && scope->IsBooleanScope())
            return const_cast<Actor*>(current);
        current = current->GetParent();
    }
    return nullptr;
}

namespace
{
    void WalkTargetScopesRecursive(Actor* current, Actor* targetRoot, Array<Actor*>& outExplicitStacks, Array<Brush*>& outImplicitBrushes)
    {
        if (current == nullptr || !current->GetIsActive())
            return;

        if (current != targetRoot)
        {
            auto scope = dynamic_cast<CSGScopeActor*>(current);
            if (scope && scope->IsBooleanScope())
            {
                outExplicitStacks.Add(current);
                return; // Stack brushes will be collected independently when the stack is built
            }
        }

        auto brush = dynamic_cast<Brush*>(current);
        if (brush && brush->CanUseCSG())
        {
            outImplicitBrushes.Add(brush);
        }

        for (int32 i = 0; i < current->Children.Count(); i++)
        {
            WalkTargetScopesRecursive(current->Children[i], targetRoot, outExplicitStacks, outImplicitBrushes);
        }
    }

    void WalkStackBrushesRecursive(Actor* current, Actor* stackRoot, Array<Brush*>& outBrushes)
    {
        if (current == nullptr || !current->GetIsActive())
            return;

        if (current != stackRoot)
        {
            auto scope = dynamic_cast<CSGScopeActor*>(current);
            if (scope)
                return; // Stop at nested stack boundary
        }

        auto brush = dynamic_cast<Brush*>(current);
        if (brush && brush->CanUseCSG())
        {
            outBrushes.Add(brush);
        }

        for (int32 i = 0; i < current->Children.Count(); i++)
        {
            WalkStackBrushesRecursive(current->Children[i], stackRoot, outBrushes);
        }
    }
}

void CSGHierarchy::CollectTargetScopes(Actor* targetRoot, Array<Actor*>& outExplicitStacks, Array<Brush*>& outImplicitBrushes)
{
    outExplicitStacks.Clear();
    outImplicitBrushes.Clear();
    if (targetRoot == nullptr)
        return;

    for (int32 i = 0; i < targetRoot->Children.Count(); i++)
    {
        WalkTargetScopesRecursive(targetRoot->Children[i], targetRoot, outExplicitStacks, outImplicitBrushes);
    }
}

void CSGHierarchy::CollectStackBrushes(Actor* stackRoot, Array<Brush*>& outBrushes)
{
    outBrushes.Clear();
    if (stackRoot == nullptr)
        return;

    for (int32 i = 0; i < stackRoot->Children.Count(); i++)
    {
        WalkStackBrushesRecursive(stackRoot->Children[i], stackRoot, outBrushes);
    }
}
