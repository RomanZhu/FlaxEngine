// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGCompilation.h"
#include "CSGStackEvaluator.h"
#include "Engine/Level/Actor.h"
#include "Engine/Level/Scene/Scene.h"

#if COMPILE_WITH_CSG_BUILDER

using namespace CSG;

bool CSGCompilation::SnapshotOperand(Brush* brush, int32 operationIndex, Operand& outOperand)
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
    if (actor != nullptr)
    {
        const auto box = actor->GetBox();
        outOperand.Bounds.Clear();
        outOperand.Bounds.Add(box.Minimum);
        outOperand.Bounds.Add(box.Maximum);
    }

    return true;
}

bool CSGCompilation::SnapshotOperands(const Array<Brush*>& brushes, Array<Operand>& outOperands)
{
    outOperands.Clear();
    outOperands.EnsureCapacity(brushes.Count());

    for (int32 i = 0; i < brushes.Count(); i++)
    {
        Operand op;
        if (SnapshotOperand(brushes[i], i, op))
        {
            outOperands.Add(op);
        }
    }

    return outOperands.HasItems();
}

bool CSGCompilation::CompileStack(const Array<Brush*>& brushes, Mesh& outMesh, StackBuildStats* stats)
{
    if (brushes.IsEmpty())
        return true;

    Array<Operand> operands;
    if (!SnapshotOperands(brushes, operands))
        return false;

    return CSGStackEvaluator::EvaluateStack(Span<const Operand>(operands.Get(), operands.Count()), outMesh, stats);
}

bool CSGCompilation::CompileTargetMeshes(Scene* scene, Mesh& outCombinedMesh)
{
    if (scene == nullptr)
        return false;

    Array<Actor*> explicitStacks;
    Array<Brush*> implicitBrushes;
    CSGHierarchy::CollectTargetScopes(scene, explicitStacks, implicitBrushes);

    if (implicitBrushes.HasItems())
    {
        Mesh implicitMesh;
        if (CompileStack(implicitBrushes, implicitMesh))
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
            if (CompileStack(stackBrushes, stackMesh))
            {
                outCombinedMesh.AppendResolvedGeometry(&stackMesh);
            }
        }
    }

    return true;
}

#endif
