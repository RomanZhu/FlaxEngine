// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Asset.h"

/// <summary>
/// Asset reference utility that doesn't add reference to that asset. Handles asset unload event.
/// </summary>
API_CLASS(InBuild) class WeakAssetReferenceBase : public IAssetReference
{
public:
    typedef Delegate<> EventType;

protected:
    Asset* _asset = nullptr;
    Guid _objectId;

public:
    /// <summary>
    /// The asset unloading event (should cleanup refs to it).
    /// </summary>
    EventType Unload;

public:
    NON_COPYABLE(WeakAssetReferenceBase);

    /// <summary>
    /// Initializes a new instance of the <see cref="WeakAssetReferenceBase"/> class.
    /// </summary>
    WeakAssetReferenceBase() = default;

    /// <summary>
    /// Finalizes an instance of the <see cref="WeakAssetReferenceBase"/> class.
    /// </summary>
    ~WeakAssetReferenceBase();

public:
    /// <summary>
    /// Gets the persistent asset object ID.
    /// </summary>
    FORCE_INLINE Guid GetID() const
    {
        return _objectId;
    }

    /// <summary>
    /// Gets managed instance object (or null if no asset set).
    /// </summary>
    FORCE_INLINE MObject* GetManagedInstance() const
    {
        return _asset ? _asset->GetOrCreateManagedInstance() : nullptr;
    }

    /// <summary>
    /// Gets the asset property value as string.
    /// </summary>
    String ToString() const;

public:
    // [IAssetReference]
    void OnAssetChanged(Asset* asset, void* caller) override;
    void OnAssetLoaded(Asset* asset, void* caller) override;
    void OnAssetUnloaded(Asset* asset, void* caller) override;

protected:
    void OnSet(Asset* asset);
    void OnSet(const Guid& objectId, const ScriptingTypeHandle& type);
};

/// <summary>
/// Asset reference utility that doesn't add reference to that asset. Handles asset unload event.
/// </summary>
template<typename T>
API_CLASS(InBuild) class WeakAssetReference : public WeakAssetReferenceBase
{
public:
    /// <summary>
    /// Initializes a new instance of the <see cref="WeakAssetReference"/> class.
    /// </summary>
    WeakAssetReference()
    {
    }

    /// <summary>
    /// Initializes a new instance of the <see cref="WeakAssetReference"/> class.
    /// </summary>
    explicit WeakAssetReference(decltype(__nullptr))
    {
    }

    /// <summary>
    /// Initializes a new instance of the <see cref="WeakAssetReference"/> class.
    /// </summary>
    /// <param name="asset">The asset to set.</param>
    WeakAssetReference(T* asset)
    {
        OnSet(asset);
    }

    WeakAssetReference(const Guid& objectId)
    {
        OnSet(objectId, T::TypeInitializer);
    }

    /// <summary>
    /// Initializes a new instance of the <see cref="WeakAssetReference"/> class.
    /// </summary>
    /// <param name="other">The other.</param>
    WeakAssetReference(const WeakAssetReference& other)
    {
        OnSet(other.Get());
        _objectId = other._objectId;
    }

    WeakAssetReference(WeakAssetReference&& other)
    {
        OnSet(other.Get());
        _objectId = other._objectId;
        other.OnSet(nullptr);
        other._objectId = Guid::Empty;
    }

    WeakAssetReference& operator=(WeakAssetReference&& other)
    {
        if (&other != this)
        {
            OnSet(other.Get());
            _objectId = other._objectId;
            other.OnSet(nullptr);
            other._objectId = Guid::Empty;
        }
        return *this;
    }

    /// <summary>
    /// Finalizes an instance of the <see cref="WeakAssetReference"/> class.
    /// </summary>
    ~WeakAssetReference()
    {
    }

public:
    FORCE_INLINE WeakAssetReference& operator=(const WeakAssetReference& other)
    {
        OnSet(other.Get());
        _objectId = other._objectId;
        return *this;
    }

    FORCE_INLINE WeakAssetReference& operator=(T* other)
    {
        OnSet((Asset*)other);
        return *this;
    }

    FORCE_INLINE WeakAssetReference& operator=(const Guid& id)
    {
        OnSet(id, T::TypeInitializer);
        return *this;
    }

    FORCE_INLINE bool operator==(T* other) const
    {
        return _asset == other;
    }

    FORCE_INLINE bool operator==(const WeakAssetReference& other) const
    {
        return _asset == other._asset;
    }

    /// <summary>
    /// Implicit conversion to the bool.
    /// </summary>
    FORCE_INLINE operator T*() const
    {
        return (T*)_asset;
    }

    /// <summary>
    /// Implicit conversion to the asset.
    /// </summary>
    FORCE_INLINE operator bool() const
    {
        return _asset != nullptr;
    }

    /// <summary>
    /// Implicit conversion to the asset.
    /// </summary>
    FORCE_INLINE T* operator->() const
    {
        return (T*)_asset;
    }

    /// <summary>
    /// Gets the asset.
    /// </summary>
    FORCE_INLINE T* Get() const
    {
        return (T*)_asset;
    }

    /// <summary>
    /// Gets the asset as a given type (static cast).
    /// </summary>
    template<typename U>
    FORCE_INLINE U* As() const
    {
        return (U*)_asset;
    }

public:
    /// <summary>
    /// Sets the asset reference.
    /// </summary>
    /// <param name="asset">The asset.</param>
    void Set(T* asset)
    {
        OnSet(asset);
    }
};

template<typename T>
uint32 GetHash(const WeakAssetReference<T>& key)
{
    return GetHash(key.GetID());
}
