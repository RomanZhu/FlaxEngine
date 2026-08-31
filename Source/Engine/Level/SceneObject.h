// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Content/AssetDatabase/Identity/GlobalAssetObjectId.h"

class SceneTicking;
class ScriptsFactory;
class Actor;
class Script;
class Joint;
class Scene;
class Prefab;
class PrefabInstanceData;
class SceneObject;
class PrefabManager;
class Level;
class SceneLoader;
class SceneObjectsFactory;

/// <summary>
/// Scene objects setup data container used for BeginPlay callback.
/// </summary>
class SceneBeginData
{
public:
    /// <summary>
    /// The joints to create after setup.
    /// </summary>
    Array<Joint*> JointsToCreate;

    /// <summary>
    /// Called when scene objects setup is done.
    /// </summary>
    void OnDone();
};

/// <summary>
/// The actors collection lookup type (id -> actor).
/// </summary>
typedef Dictionary<Guid, Actor*, HeapAllocation> ActorsLookup;

#define DECLARE_SCENE_OBJECT(type) \
    DECLARE_SCRIPTING_TYPE(type)

#define DECLARE_SCENE_OBJECT_ABSTRACT(type) \
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(type); \
    static type* Spawn(const SpawnParams& params) { return nullptr; } \
    explicit type(const SpawnParams& params)

#define DECLARE_SCENE_OBJECT_NO_SPAWN(type) \
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(type)

#if USE_EDITOR

/// <summary>
/// Dense persistent sibling position used by external actor storage.
/// </summary>
struct FLAXENGINE_API ExternalSiblingOrderKey
{
    Array<uint16, InlinedAllocation<4>> Digits;

    FORCE_INLINE bool IsValid() const
    {
        return Digits.HasItems();
    }

    int32 Compare(const ExternalSiblingOrderKey& other) const;
    String ToString() const;

    static bool TryParse(const StringView& text, ExternalSiblingOrderKey& result);
    static ExternalSiblingOrderKey FromLegacy(int64 value);
    static ExternalSiblingOrderKey CreateBetween(const ExternalSiblingOrderKey* previous, const ExternalSiblingOrderKey* next, const Guid& objectId);
};

#endif

/// <summary>
/// Base class for objects that are parts of the scene (actors and scripts).
/// </summary>
API_CLASS(Abstract, NoSpawn) class FLAXENGINE_API SceneObject : public ScriptingObject, public ISerializable
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(SceneObject);
    friend PrefabInstanceData;
    friend PrefabManager;
    friend Actor;
    friend Level;
    friend SceneLoader;
    friend SceneObjectsFactory;
    friend ScriptsFactory;
    friend SceneTicking;
public:
    typedef ScriptingObject Base;

    // Scene Object lifetime flow:
    // - Create
    // - Is created from code:
    //    - Post Spawn
    // - otherwise:
    //   - Deserialize (more than once for prefab instances)
    //   - Post Load
    // - Begin Play
    // - End Play
    // - Destroy

protected:
    Actor* _parent;
    AssetGuid _persistentSourceAsset;
    LocalFileId _localFileId;
    Guid _prefabID;
    Guid _prefabObjectID;
    LocalFileId _prefabObjectFileId;

#if USE_EDITOR
    ExternalSiblingOrderKey _externalSiblingOrderKey;
    Guid _externalSiblingOrderParentId;
    int64 _externalLegacyOrderInParent;
    bool _hasExternalLegacyOrderInParent;
#endif

    /// <summary>
    /// Initializes a new instance of the <see cref="SceneObject"/> class.
    /// </summary>
    /// <param name="params">The object initialization parameters.</param>
    SceneObject(const SpawnParams& params);

public:
    /// <summary>
    /// Finalizes an instance of the <see cref="SceneObject"/> class.
    /// </summary>
    ~SceneObject();

public:
    /// <summary>
    /// Determines whether object is during play (spawned/loaded and fully initialized).
    /// </summary>
    API_PROPERTY() FORCE_INLINE bool IsDuringPlay() const
    {
        return (Flags & ObjectFlags::IsDuringPlay) == ObjectFlags::IsDuringPlay;
    }

    /// <summary>
    /// Returns true if object has a parent assigned.
    /// </summary>
    API_PROPERTY() FORCE_INLINE bool HasParent() const
    {
        return _parent != nullptr;
    }

    /// <summary>
    /// Gets the parent actor (or null if object has no parent).
    /// </summary>
    API_PROPERTY(Attributes="HideInEditor")
    FORCE_INLINE Actor* GetParent() const
    {
        return _parent;
    }

    /// <summary>
    /// Sets the parent actor.
    /// </summary>
    /// <param name="value">The new parent.</param>
    API_PROPERTY() FORCE_INLINE void SetParent(Actor* value)
    {
        SetParent(value, true);
    }

    /// <summary>
    /// Sets the parent actor.
    /// </summary>
    /// <param name="value">The new parent.</param>
    /// <param name="canBreakPrefabLink">True if can break prefab link on changing the parent.</param>
    API_FUNCTION() virtual void SetParent(Actor* value, bool canBreakPrefabLink) = 0;

#if USE_EDITOR

    const ExternalSiblingOrderKey& GetExternalSiblingOrderKey() const;
    bool HasExternalSiblingOrderKeyForCurrentParent() const;
    bool HasExternalLegacyOrderInParent() const;
    int64 GetExternalOrderInParent() const;
    void SetExternalOrderInParent(int64 value);
    void SetExternalSiblingOrderKey(const ExternalSiblingOrderKey& value);

#endif

    /// <summary>
    /// Gets the scene object ID.
    /// </summary>
    /// <returns>The scene object ID.</returns>
    virtual const Guid& GetSceneObjectId() const = 0;

    /// <summary>
    /// Gets the stable authored identifier of this actor or component inside its owning scene document.
    /// Runtime scripting GUIDs are deliberately not persistent identity.
    /// </summary>
    API_PROPERTY(Attributes="HideInEditor") FORCE_INLINE int64 GetLocalFileId() const
    {
        return _localFileId;
    }

    /// <summary>
    /// Gets the persistent global identity for this scene or prefab object.
    /// </summary>
    API_PROPERTY(Attributes="HideInEditor") GlobalAssetObjectId GetGlobalObjectId() const;

    /// <summary>
    /// Resolves a persistent scene or prefab object identity if its owning scene is loaded.
    /// Missing objects preserve their original identity at the call site and resolve to null.
    /// </summary>
    API_FUNCTION() static SceneObject* ResolveGlobalObjectId(API_PARAM(Ref) const GlobalAssetObjectId& objectId);

    /// <summary>
    /// Gets zero-based index in parent actor children list (scripts or child actors).
    /// </summary>
    /// <returns>The order in parent.</returns>
    API_PROPERTY(Attributes="HideInEditor")
    virtual int32 GetOrderInParent() const = 0;

    /// <summary>
    /// Sets zero-based index in parent actor children list (scripts or child actors).
    /// </summary>
    /// <param name="index">The new order in parent.</param>
    API_PROPERTY() virtual void SetOrderInParent(int32 index) = 0;

public:
    /// <summary>
    /// Gets a value indicating whether this object has a valid linkage to the prefab asset.
    /// </summary>
    API_PROPERTY() FORCE_INLINE bool HasPrefabLink() const
    {
        return _prefabID.IsValid();
    }

    /// <summary>
    /// Gets the prefab asset ID. Empty if no prefab link exists.
    /// </summary>
    API_PROPERTY() FORCE_INLINE Guid GetPrefabID() const
    {
        return _prefabID;
    }

    /// <summary>
    /// Gets the ID of the object within a prefab that is used for synchronization with this object. Empty if no prefab link exists.
    /// </summary>
    API_PROPERTY() FORCE_INLINE Guid GetPrefabObjectID() const
    {
        return _prefabObjectID;
    }

    /// <summary>
    /// Gets the stable authored identifier of the linked object inside the prefab source.
    /// </summary>
    API_PROPERTY(Attributes="HideInEditor") FORCE_INLINE int64 GetPrefabObjectFileId() const
    {
        return _prefabObjectFileId;
    }

    /// <summary>
    /// Links scene object instance to the prefab asset and prefab object. Warning! This applies to the only this object (not scripts or child actors).
    /// </summary>
    /// <param name="prefabId">The prefab asset identifier.</param>
    /// <param name="prefabObjectId">The prefab object identifier.</param>
    API_FUNCTION(Attributes="NoAnimate") virtual void LinkPrefab(const Guid& prefabId, const Guid& prefabObjectId);

    /// <summary>
    /// Links this instance to an authored prefab object by its stable local file ID.
    /// </summary>
    void LinkPrefabObject(const Guid& prefabId, LocalFileId prefabObjectFileId);

    /// <summary>
    /// Breaks the prefab linkage for this object, all its scripts, and all child actors.
    /// </summary>
    API_FUNCTION(Attributes="NoAnimate")
    virtual void BreakPrefabLink();

    static Guid MakeRuntimeObjectId(const Guid& sourceAssetId, LocalFileId localFileId, GlobalObjectKind kind = GlobalObjectKind::SceneObject, LocalFileId prefabInstanceFileId = 0);
    static LocalFileId MakeLocalFileId(const Guid& runtimeSeed);

    FORCE_INLINE void SetPersistentDocumentIdentity(const AssetGuid& sourceAsset, LocalFileId localFileId)
    {
        _persistentSourceAsset = sourceAsset;
        _localFileId = localFileId;
    }

    /// <summary>
    /// Gets the path containing name of this object and all parent objects in tree hierarchy separated with custom separator character (/ by default). Can be used to identify this object in logs.
    /// </summary>
    /// <param name="separatorChar">The character to separate the names.</param>
    /// <returns>The full name path.</returns>
    API_FUNCTION() String GetNamePath(Char separatorChar = '/') const;

public:
    /// <summary>
    /// Called after object loading or spawning to initialize the object (eg. call OnAwake for scripts) but before BeginPlay. Initialization should be performed only within a single SceneObject (use BeginPlay to initialize with a scene).
    /// </summary>
    virtual void Initialize() = 0;

    /// <summary>
    /// Called when adding object to the game.
    /// </summary>
    /// <param name="data">The initialization data (e.g. used to collect joints to link after begin).</param>
    virtual void BeginPlay(SceneBeginData* data) = 0;

    /// <summary>
    /// Called when removing object from the game.
    /// </summary>
    virtual void EndPlay() = 0;

public:
    // [ISerializable]
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;
};
