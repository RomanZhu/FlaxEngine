// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/ISerializable.h"
#include "Engine/Core/Math/Triangle.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Physics/CollisionData.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Content/Assets/RawDataAsset.h"
#include "Engine/Content/Assets/Model.h"

namespace CSG
{
    /// <summary>
    /// Reusable CSG compiled output container (models, raw surface queries, collision).
    /// Used by both legacy Scene CSG output and CSGModel actor outputs.
    /// </summary>
    class FLAXENGINE_API CSGCompiledData : public ISerializable
    {
    public:
        /// <summary>
        /// The persisted CSG model mesh.
        /// </summary>
        AssetReference<Model> Model;

        /// <summary>
        /// The transient model used for live CSG rendering.
        /// </summary>
        AssetReference<::Model> PreviewModel;

        /// <summary>
        /// The inactive transient model reused by the next live CSG update.
        /// </summary>
        AssetReference<::Model> PreviewModelCache;

        /// <summary>
        /// The CSG mesh raw data.
        /// </summary>
        AssetReference<RawDataAsset> Data;

        /// <summary>
        /// The CSG mesh collision data.
        /// </summary>
        AssetReference<CollisionData> CollisionData;

        /// <summary>
        /// The brush data locations lookup for faster searching through Data container.
        /// </summary>
        Dictionary<Guid, int32> DataBrushLocations;

        /// <summary>
        /// The post CSG build action. Called by the CSGBuilder after CSG mesh building completes.
        /// </summary>
        Action PostCSGBuild;

    public:
        CSGCompiledData();
        ~CSGCompiledData();

        /// <summary>
        /// Determines whether this container has valid CSG model data linked.
        /// </summary>
        bool HasData() const;

        /// <summary>
        /// Gets the model to use for rendering CSG geometry.
        /// </summary>
        FORCE_INLINE ::Model* GetModelForRendering() const
        {
            return PreviewModel ? PreviewModel.Get() : Model.Get();
        }

        /// <summary>
        /// Clears transient preview models.
        /// </summary>
        void ClearTransientPreview();

        /// <summary>
        /// Clears all linked assets and caches.
        /// </summary>
        void ClearAll();

    public:
        struct SurfaceData
        {
            Array<Triangle> Triangles;

            bool Intersects(const Ray& ray, Real& distance, Vector3& normal) const;
        };

        /// <summary>
        /// Tries to get the brush surface data.
        /// </summary>
        /// <param name="brushId">The brush identifier.</param>
        /// <param name="brushSurfaceIndex">Index of the brush surface.</param>
        /// <param name="outData">The output data.</param>
        /// <returns>True if found data, otherwise false.</returns>
        bool TryGetSurfaceData(const Guid& brushId, int32 brushSurfaceIndex, SurfaceData& outData);

    protected:
        void OnDataChanged();

    public:
        // [ISerializable]
        void Serialize(SerializeStream& stream, const void* otherObj) override;
        void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;
    };
}
