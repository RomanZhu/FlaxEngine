// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "CSGScopeActor.h"
#include "Engine/Level/CSG/CSGCompiledData.h"

class StaticModel;
class MeshCollider;

/// <summary>
/// A live CSG authoring assembly and independent generated-output boundary.
/// Child brushes and stacks remain editable source geometry.
/// The generated model, raw surface data, and collision are derived outputs.
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
    /// Requests CSG geometry preview rebuild for this model.
    /// </summary>
    /// <param name="timeoutMs">The timeout to wait before building CSG (in milliseconds).</param>
    API_FUNCTION() void BuildCSG(float timeoutMs = 50) const;

    /// <summary>
    /// Synchronously compiles and persists CSG model output.
    /// </summary>
    /// <param name="ownerAssetId">The owning asset ID (e.g. Prefab ID) for asset-owned output, or empty for scene-owned output.</param>
    /// <returns>True if successfully persisted, false otherwise.</returns>
    API_FUNCTION() bool PersistCSG(const Guid& ownerAssetId = Guid::Empty) const;

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
