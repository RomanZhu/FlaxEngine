// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Content/Content.h"

extern FLAXENGINE_API ScriptingObject* FindObject(const Guid& id, MClass* type);

/// <summary>
/// The scripting object soft reference. Objects gets referenced on use (ID reference is resolving it).
/// </summary>
class FLAXENGINE_API SoftObjectReferenceBase
{
protected:

    ScriptingObject* _object = nullptr;
    Guid _id = Guid::Empty;
    mutable AssetObjectId _objectId;

public:

    /// <summary>
    /// Action fired when reference gets changed.
    /// </summary>
    Delegate<> Changed;

public:
    NON_COPYABLE(SoftObjectReferenceBase);

    /// <summary>
    /// Initializes a new instance of the <see cref="SoftObjectReferenceBase"/> class.
    /// </summary>
    SoftObjectReferenceBase()
    {
    }

    /// <summary>
    /// Finalizes an instance of the <see cref="SoftObjectReference"/> class.
    /// </summary>
    ~SoftObjectReferenceBase()
    {
        ScriptingObject* obj = _object;
        if (obj)
        {
            _object = nullptr;
            obj->Deleted.Unbind<SoftObjectReferenceBase, &SoftObjectReferenceBase::OnDeleted>(this);
        }
    }

public:

    /// <summary>
    /// Gets the object ID.
    /// </summary>
    Guid GetID() const
    {
        return _id;
    }

    /// <summary>Gets the persistent identity when this reference targets an asset object.</summary>
    const AssetObjectId& GetObjectId() const
    {
        if (!_objectId.IsValid() && _id.IsValid())
            Content::GetAssetObjectId(_id, _objectId);
        return _objectId;
    }

protected:

    void OnSet(ScriptingObject* object)
    {
        if (_object == object)
            return;
        if (_object)
            _object->Deleted.Unbind<SoftObjectReferenceBase, &SoftObjectReferenceBase::OnDeleted>(this);
        _object = object;
        _id = object ? object->GetID() : Guid::Empty;
        _objectId = AssetObjectId();
        if (_id.IsValid())
            Content::GetAssetObjectId(_id, _objectId);
        if (object)
            object->Deleted.Bind<SoftObjectReferenceBase, &SoftObjectReferenceBase::OnDeleted>(this);
        Changed();
    }

    void OnSet(const Guid& id)
    {
        if (_id == id)
            return;
        if (_object)
            _object->Deleted.Unbind<SoftObjectReferenceBase, &SoftObjectReferenceBase::OnDeleted>(this);
        _object = nullptr;
        _id = id;
        _objectId = AssetObjectId();
        if (_id.IsValid())
            Content::GetAssetObjectId(_id, _objectId);
        Changed();
    }

    void OnSet(const AssetObjectId& id)
    {
        if (_objectId == id)
            return;
        if (_object)
            _object->Deleted.Unbind<SoftObjectReferenceBase, &SoftObjectReferenceBase::OnDeleted>(this);
        _object = nullptr;
        _objectId = id;
        _id = id.LocalId == 1 ? id.Guid : Guid::Empty;
        Changed();
    }

    void OnResolve(MClass* type)
    {
        ASSERT(!_object);
        _object = _objectId.IsValid() ? Content::LoadAsync(_objectId, type) : FindObject(_id, type);
        if (_object)
        {
            _id = _object->GetID();
            _object->Deleted.Bind<SoftObjectReferenceBase, &SoftObjectReferenceBase::OnDeleted>(this);
        }
    }

    void OnDeleted(ScriptingObject* obj)
    {
        ASSERT(_object == obj);
        _object->Deleted.Unbind<SoftObjectReferenceBase, &SoftObjectReferenceBase::OnDeleted>(this);
        _object = nullptr;
        _id = Guid::Empty;
        _objectId = AssetObjectId();
        Changed();
    }
};

/// <summary>
/// The scripting object soft reference. Objects gets referenced on use (ID reference is resolving it).
/// </summary>
template<typename T>
API_CLASS(InBuild) class SoftObjectReference : public SoftObjectReferenceBase
{
public:

    typedef SoftObjectReference<T> Type;

public:

    /// <summary>
    /// Initializes a new instance of the <see cref="SoftObjectReference"/> class.
    /// </summary>
    SoftObjectReference()
    {
    }

    /// <summary>
    /// Initializes a new instance of the <see cref="SoftObjectReference"/> class.
    /// </summary>
    /// <param name="obj">The object to link.</param>
    SoftObjectReference(T* obj)
    {
        OnSet((ScriptingObject*)obj);
    }

    /// <summary>
    /// Initializes a new instance of the <see cref="SoftObjectReference"/> class.
    /// </summary>
    /// <param name="other">The other property.</param>
    SoftObjectReference(const SoftObjectReference& other)
    {
        if (other.GetObjectId().IsValid())
            OnSet(other.GetObjectId());
        else
            OnSet(other.GetID());
    }

    /// <summary>
    /// Initializes a new instance of the <see cref="SoftObjectReference"/> class.
    /// </summary>
    /// <param name="other">The other property.</param>
    SoftObjectReference(SoftObjectReference&& other)
    {
        if (other.GetObjectId().IsValid())
            OnSet(other.GetObjectId());
        else
            OnSet(other.GetID());
        other.OnSet(nullptr);
    }

    /// <summary>
    /// Finalizes an instance of the <see cref="SoftObjectReference"/> class.
    /// </summary>
    ~SoftObjectReference()
    {
    }

public:

    FORCE_INLINE bool operator==(T* other)
    {
        return Get() == other;
    }
    FORCE_INLINE bool operator==(const SoftObjectReference& other)
    {
        return GetID() == other.GetID();
    }
    FORCE_INLINE bool operator!=(T* other)
    {
        return Get() != other;
    }
    FORCE_INLINE bool operator!=(const SoftObjectReference& other)
    {
        return GetID() != other.GetID();
    }
    SoftObjectReference& operator=(const SoftObjectReference& other)
    {
        if (this != &other)
        {
            if (other.GetObjectId().IsValid())
                OnSet(other.GetObjectId());
            else
                OnSet(other.GetID());
        }
        return *this;
    }
    SoftObjectReference& operator=(SoftObjectReference&& other)
    {
        if (this != &other)
        {
            if (other.GetObjectId().IsValid())
                OnSet(other.GetObjectId());
            else
                OnSet(other.GetID());
            other.OnSet(nullptr);
        }
        return *this;
    }
    FORCE_INLINE SoftObjectReference& operator=(const T& other)
    {
        OnSet(&other);
        return *this;
    }
    FORCE_INLINE SoftObjectReference& operator=(T* other)
    {
        OnSet(other);
        return *this;
    }
    FORCE_INLINE SoftObjectReference& operator=(const Guid& id)
    {
        OnSet(id);
        return *this;
    }
    FORCE_INLINE SoftObjectReference& operator=(const AssetObjectId& id)
    {
        OnSet(id);
        return *this;
    }
    FORCE_INLINE operator T*() const
    {
        return (T*)Get();
    }
    FORCE_INLINE operator bool() const
    {
        return Get() != nullptr;
    }
    FORCE_INLINE T* operator->() const
    {
        return (T*)Get();
    }
    template<typename U>
    FORCE_INLINE U* As() const
    {
        return static_cast<U*>(Get());
    }

public:
    
    /// <summary>
    /// Gets the object (or null if unassigned).
    /// </summary>
    T* Get() const
    {
        if (!_object)
            const_cast<SoftObjectReference*>(this)->OnResolve(T::GetStaticClass());
        return (T*)_object;
    }

    /// <summary>
    /// Gets managed instance object (or null if no object linked).
    /// </summary>
    MObject* GetManagedInstance() const
    {
        auto object = Get();
        return object ? object->GetOrCreateManagedInstance() : nullptr;
    }

    /// <summary>
    /// Determines whether object is assigned and managed instance of the object is alive.
    /// </summary>
    bool HasManagedInstance() const
    {
        auto object = Get();
        return object && object->HasManagedInstance();
    }

    /// <summary>
    /// Gets the managed instance object or creates it if missing or null if not assigned.
    /// </summary>
    MObject* GetOrCreateManagedInstance() const
    {
        auto object = Get();
        return object ? object->GetOrCreateManagedInstance() : nullptr;
    }

    /// <summary>
    /// Sets the object.
    /// </summary>
    /// <param name="id">The object ID. Uses Scripting to find the registered object of the given ID.</param>
    FORCE_INLINE void Set(const Guid& id)
    {
        OnSet(id);
    }

    /// <summary>Sets an exact persistent asset object identity.</summary>
    FORCE_INLINE void Set(const AssetObjectId& id)
    {
        OnSet(id);
    }

    /// <summary>
    /// Sets the object.
    /// </summary>
    /// <param name="object">The object.</param>
    FORCE_INLINE void Set(T* object)
    {
        OnSet(object);
    }
};

template<typename T>
uint32 GetHash(const SoftObjectReference<T>& key)
{
    return GetHash(key.GetID());
}
