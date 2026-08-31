// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "GlobalAssetObjectId.h"
#include "Engine/Core/ISerializable.h"

namespace Serialization
{
    bool FLAXENGINE_API ShouldSerialize(const AssetGuid& value, const void* otherObj);
    void FLAXENGINE_API Serialize(ISerializable::SerializeStream& stream, const AssetGuid& value, const void* otherObj);
    void FLAXENGINE_API Deserialize(ISerializable::DeserializeStream& stream, AssetGuid& value, ISerializeModifier* modifier);

    bool FLAXENGINE_API ShouldSerialize(const AssetObjectId& value, const void* otherObj);
    void FLAXENGINE_API Serialize(ISerializable::SerializeStream& stream, const AssetObjectId& value, const void* otherObj);
    void FLAXENGINE_API Deserialize(ISerializable::DeserializeStream& stream, AssetObjectId& value, ISerializeModifier* modifier);

    bool FLAXENGINE_API ShouldSerialize(const GlobalAssetObjectId& value, const void* otherObj);
    void FLAXENGINE_API Serialize(ISerializable::SerializeStream& stream, const GlobalAssetObjectId& value, const void* otherObj);
    void FLAXENGINE_API Deserialize(ISerializable::DeserializeStream& stream, GlobalAssetObjectId& value, ISerializeModifier* modifier);
}
