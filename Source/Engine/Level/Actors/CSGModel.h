// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "CSGScopeActor.h"
#include "Engine/Level/CSG/CSGCompiledData.h"

class StaticModel;
class MeshCollider;

/// <summary>
/// A CSG scope actor that establishes an independent generated output boundary.
/// All brushes and stacks inside this model compile into this actor's own Model and CollisionData
/// in model-local coordinate space.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/CSG/CSG Model\"), ActorToolbox(\"CSG\")")
class FLAXENGINE_API CSGModel : public CSGScopeActor
{
    DECLARE_SCENE_OBJECT(CSGModel);

public:
    /// <summary>
    /// The compiled CSG data for this model (model, preview model, raw data, collision).
    /// </summary>
    CSG::CSGCompiledData CSGData;

public:
    // [CSGScopeActor]
    CSGScopeKind GetCSGScopeKind() const override
    {
        return CSGScopeKind::ModelOutput;
    }

    /// <summary>
    /// Requests CSG geometry rebuild for this model.
    /// </summary>
    /// <param name="timeoutMs">The timeout to wait before building CSG (in milliseconds).</param>
    API_FUNCTION() void BuildCSG(float timeoutMs = 50) const;

    /// <summary>
    /// Tries to get the generated static model component.
    /// </summary>
    StaticModel* TryGetCsgModel() const;

    /// <summary>
    /// Tries to get the generated mesh collider component.
    /// </summary>
    MeshCollider* TryGetCsgCollider() const;

private:
    void CreateCsgModel();
    void CreateCsgCollider();
    void OnCsgModelChanged();
    void OnCsgCollisionDataChanged();
    void OnCSGBuildEnd();

public:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;
};
