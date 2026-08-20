// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGCompilation.h"
#include "CSGStackEvaluator.h"
#include "Engine/Level/Actor.h"
#include "Engine/Level/Actors/CSGScopeActor.h"

#if COMPILE_WITH_CSG_BUILDER

using namespace CSG;

bool CSGCompilation::SnapshotOperand(Brush* brush, int32 operationIndex, Operand& outOperand, const Transform* targetTransform)
{
    if (brush == nullptr)
        return false;

    outOperand.SourceBrush = brush;
    outOperand.BrushId = brush->GetBrushID();
    outOperand.Mode = brush->GetBrushMode();
    outOperand.FlipNormals = brush->GetBrushFlipNormals();
    outOperand.OperationIndex = operationIndex;
    brush->GetSurfaces(outOperand.Surfaces);

    if (outOperand.Surfaces.Count() < 4)
        return false;

    auto actor = dynamic_cast<Actor*>(brush);
    if (targetTransform != nullptr && !targetTransform->IsIdentity())
    {
        for (int32 i = 0; i < outOperand.Surfaces.Count(); i++)
        {
            auto& surface = outOperand.Surfaces[i];
            Vector3 p0World = surface.Normal * surface.D;
            Vector3 p0Local = targetTransform->WorldToLocal(p0World);
            Vector3 normalLocal = targetTransform->WorldToLocalVector(surface.Normal);
            normalLocal.Normalize();
            surface.Normal = normalLocal;
            surface.D = Vector3::Dot(normalLocal, p0Local);
        }

        if (actor != nullptr)
        {
            Vector3 corners[8];
            actor->GetBox().GetCorners(corners);
            outOperand.Bounds.Clear();
            for (int32 c = 0; c < 8; c++)
            {
                outOperand.Bounds.Add(targetTransform->WorldToLocal(corners[c]));
            }
        }
    }
    else if (actor != nullptr)
    {
        const auto box = actor->GetBox();
        outOperand.Bounds.Clear();
        outOperand.Bounds.Add(box.Minimum);
        outOperand.Bounds.Add(box.Maximum);
    }

    return true;
}

bool CSGCompilation::SnapshotOperands(const Array<Brush*>& brushes, Array<Operand>& outOperands, const Transform* targetTransform)
{
    outOperands.Clear();
    outOperands.EnsureCapacity(brushes.Count());

    for (int32 i = 0; i < brushes.Count(); i++)
    {
        Operand op;
        if (SnapshotOperand(brushes[i], i, op, targetTransform))
        {
            outOperands.Add(op);
        }
    }

    return outOperands.HasItems();
}

bool CSGCompilation::CompileStack(const Array<Brush*>& brushes, Mesh& outMesh, const Transform* targetTransform, StackBuildStats* stats)
{
    if (brushes.IsEmpty())
        return true;

    Array<Operand> operands;
    if (!SnapshotOperands(brushes, operands, targetTransform))
        return false;

    return CSGStackEvaluator::EvaluateStack(Span<const Operand>(operands.Get(), operands.Count()), outMesh, stats);
}

bool CSGCompilation::CompileTargetMeshes(Actor* targetRoot, Mesh& outCombinedMesh)
{
    if (targetRoot == nullptr)
        return false;

    const Transform* targetTransform = nullptr;
    Transform localTransform;
    auto scope = dynamic_cast<CSGScopeActor*>(targetRoot);
    if (scope && scope->IsOutputScope())
    {
        localTransform = targetRoot->GetTransform();
        targetTransform = &localTransform;
    }

    Array<Actor*> explicitStacks;
    Array<Brush*> implicitBrushes;
    CSGHierarchy::CollectTargetScopes(targetRoot, explicitStacks, implicitBrushes);

    if (implicitBrushes.HasItems())
    {
        Mesh implicitMesh;
        if (CompileStack(implicitBrushes, implicitMesh, targetTransform))
        {
            outCombinedMesh.AppendResolvedGeometry(&implicitMesh);
        }
    }

    for (int32 i = 0; i < explicitStacks.Count(); i++)
    {
        Array<Brush*> stackBrushes;
        CSGHierarchy::CollectStackBrushes(explicitStacks[i], stackBrushes);
        if (stackBrushes.HasItems())
        {
            Mesh stackMesh;
            if (CompileStack(stackBrushes, stackMesh, targetTransform))
            {
                outCombinedMesh.AppendResolvedGeometry(&stackMesh);
            }
        }
    }

    return true;
}

#endif
