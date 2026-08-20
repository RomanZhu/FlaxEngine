// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGModel.h"
#include "Engine/CSG/CSGBuilder.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Physics/Colliders/MeshCollider.h"
#include "Engine/Serialization/Serialization.h"

#define CSG_MODEL_NAME TEXT("CSG.Model")
#define CSG_COLLIDER_NAME TEXT("CSG.Collider")

CSGModel::CSGModel(const SpawnParams& params)
    : CSGScopeActor(params)
{
    _name = TEXT("CSG Model");

    CSGData.CollisionData.Changed.Bind<CSGModel, &CSGModel::OnCsgCollisionDataChanged>(this);
    CSGData.Model.Changed.Bind<CSGModel, &CSGModel::OnCsgModelChanged>(this);
    CSGData.PostCSGBuild.Bind<CSGModel, &CSGModel::OnCSGBuildEnd>(this);
}

void CSGModel::BuildCSG(float timeoutMs) const
{
#if COMPILE_WITH_CSG_BUILDER
    // Rebuild target model via CSG builder in preview mode
    CSG::Builder::Build(const_cast<CSGModel*>(this), timeoutMs, CSG::ModelBuildIntent::Preview);
#endif
}

bool CSGModel::PersistCSG(const Guid& ownerAssetId) const
{
#if COMPILE_WITH_CSG_BUILDER
    return CSG::Builder::Persist(const_cast<CSGModel*>(this), ownerAssetId);
#else
    return false;
#endif
}

StaticModel* CSGModel::TryGetCsgModel() const
{
    for (int32 i = 0; i < Children.Count(); i++)
    {
        auto model = dynamic_cast<StaticModel*>(Children[i]);
        if (model && model->GetName() == CSG_MODEL_NAME)
            return model;
    }
    return nullptr;
}

MeshCollider* CSGModel::TryGetCsgCollider() const
{
    for (int32 i = 0; i < Children.Count(); i++)
    {
        auto collider = dynamic_cast<MeshCollider*>(Children[i]);
        if (collider && collider->GetName() == CSG_COLLIDER_NAME)
            return collider;
    }
    return nullptr;
}

void CSGModel::CreateCsgModel()
{
    auto result = New<StaticModel>();
    result->SetStaticFlags(StaticFlags::FullyStatic);
    result->SetName(String(CSG_MODEL_NAME));
    result->Model = CSGData.GetModelForRendering();
    result->HideFlags |= HideFlags::FullyHidden;
    result->SetParent(this, false, false);
}

void CSGModel::CreateCsgCollider()
{
    auto result = New<MeshCollider>();
    result->SetStaticFlags(StaticFlags::FullyStatic);
    result->SetName(String(CSG_COLLIDER_NAME));
    result->CollisionData = CSGData.CollisionData;
    result->HideFlags |= HideFlags::FullyHidden;
    result->SetParent(this, false, false);
}

void CSGModel::OnCsgModelChanged()
{
    if (!IsDuringPlay())
        return;

    auto model = TryGetCsgModel();
    if (model)
    {
        model->Model = CSGData.GetModelForRendering();
    }
    else if (CSGData.GetModelForRendering())
    {
        CreateCsgModel();
    }
}

void CSGModel::OnCsgCollisionDataChanged()
{
    if (!IsDuringPlay())
        return;

    auto collider = TryGetCsgCollider();
    if (collider)
    {
        collider->CollisionData = CSGData.CollisionData;
    }
    else if (CSGData.CollisionData)
    {
        CreateCsgCollider();
    }
}

void CSGModel::OnCSGBuildEnd()
{
    auto output = CSGData.GetModelForRendering();
    auto csgModel = TryGetCsgModel();
    if (csgModel)
    {
        csgModel->HideFlags |= HideFlags::FullyHidden;
        csgModel->Model = output;
    }
    else if (output)
    {
        CreateCsgModel();
    }

    auto collider = TryGetCsgCollider();
    if (collider)
    {
        collider->HideFlags |= HideFlags::FullyHidden;
        collider->CollisionData = CSGData.CollisionData;
    }
    else if (CSGData.CollisionData)
    {
        CreateCsgCollider();
    }
}

void CSGModel::OnEnable()
{
    CSGScopeActor::OnEnable();

    if (CSGData.CollisionData && TryGetCsgCollider() == nullptr)
        CreateCsgCollider();
    if (CSGData.GetModelForRendering() && TryGetCsgModel() == nullptr)
        CreateCsgModel();
}

void CSGModel::OnDisable()
{
    CSGScopeActor::OnDisable();
}

void CSGModel::Serialize(SerializeStream& stream, const void* otherObj)
{
    CSGScopeActor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(CSGModel);

    if (CSGData.HasData())
    {
        stream.Key("CSG");
        stream.Object(&CSGData, other ? &other->CSGData : nullptr);
    }
}

void CSGModel::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    CSGScopeActor::Deserialize(stream, modifier);

    CSGData.DeserializeIfExists(stream, "CSG", modifier);
}
